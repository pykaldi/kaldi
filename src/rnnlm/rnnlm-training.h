// rnnlm/rnnlm-training.h

// Copyright 2017  Johns Hopkins University (author: Daniel Povey)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#ifndef KALDI_RNNLM_RNNLM_TRAIN_H_
#define KALDI_RNNLM_RNNLM_TRAIN_H_

#include <thread>
#include "rnnlm/rnnlm-core-training.h"
#include "rnnlm/rnnlm-embedding-training.h"
#include "rnnlm/rnnlm-utils.h"
#include "util/kaldi-semaphore.h"


namespace kaldi {
namespace rnnlm {


struct RnnlmTrainerOptions {
  // rnnlm_rxfilename must be supplied, via --read-rnnlm option.
  std::string rnnlm_rxfilename;
  // For now, rnnlm_wxfilename must be supplied (later we could make it possible
  // to train the embedding matrix without training the RNNLM itself, if there
  // is a need).
  std::string rnnlm_wxfilename;
  // embedding_rxfilename must be supplied, via --read-embedding option.
  std::string embedding_rxfilename;
  std::string embedding_wxfilename;
  std::string word_features_rxfilename;

  RnnlmCoreTrainerOptions core_config;
  RnnlmEmbeddingTrainerOptions embedding_config;


  void Register(OptionsItf *po) {
    po->Register("read-rnnlm", &rnnlm_rxfilename,
                 "Read RNNLM from this location (e.g. 0.raw).  Must be supplied.");
    po->Register("write-rnnlm", &rnnlm_wxfilename,
                 "Write RNNLM to this location (e.g. 1.raw)."
                 "If not supplied, the core RNNLM is not trained "
                 "(but other parts of the model might be.");
    po->Register("read-embedding", &embedding_rxfilename,
                 "Location to read dense (feature or word) embedding matrix, "
                 "of dimension (num-words or num-features) by (embedding-dim).");
    po->Register("write-embedding", &embedding_wxfilename,
                 "Location to write embedding matrix (c.f. --read-embedding). "
                 "If not provided, the embedding will not be trained.");
    po->Register("read-sparse-word-features", &word_features_rxfilename,
                 "Location to read sparse word-feature matrix, e.g. "
                 "word_feats.txt.  Format is lines like: '1  30 1.0 516 1.0':"
                 "starting with word-index, then a list of pairs "
                 "(feature-index, value) only including nonzero features. "
                 "This will usually be determined in an ad-hoc way based on "
                 "letters and other hand-built features; it's not trainable."
                 " If present, the embedding matrix read via --read-embedding "
                 "will be interpreted as a feature-embedding matrix.");

    // register the core RNNLM training options options with the prefix "rnnlm",
    // so they will appear as --rnnlm.max-change and the like.  This is done
    // with a prefix because later we may add a neural net to transform the word
    // embedding, and it would have options that would have a name conflict with
    // some of these options.
    ParseOptions core_opts("rnnlm", po);
    core_config.Register(&core_opts);

    ParseOptions embedding_opts("embedding", po);
    core_config.Register(&embedding_opts);
  }

  // returns true if the combination of arguments makes sense, otherwise prints
  // a warning and returns false (the user can then call PrintUsage()).
  bool HasRequiredArgs();
};

/*
  The class RnnlmTrainer is for training an RNNLM (one individual training job, not
  the top-level logic about learning rate schedules, parameter averaging, and the
  like); it contains the most of the logic that the command-line program rnnlm-train
  implements.
*/

class RnnlmTrainer {
 public:
  // The constructor reads in any files we need to read in, initializes members,
  // and starts the background thread that computes the 'derived' parameters of
  // an eg.
  // It retains a reference to 'config'.
  RnnlmTrainer(const RnnlmTrainerOptions &config);

  // Train on one example.  The example is provided as a pointer because we
  // acquire it destructively, via Swap().  Note: this function doesn't
  // actually train on this eg; what it does is to train on the previous
  // example, and provide this eg to the background thread that computes the
  // derived parameters of the eg.
  void Train(RnnlmExample *minibatch);


  ~RnnlmTrainer();

 private:

  int32 VocabSize();

  /// This function contains the actual training code, it's called from Train();
  /// it trains on minibatch_previous_.
  void TrainInternal();

  /// This function works out the word-embedding matrix for the minibatch we're
  /// training on (previous_minibatch_).  The word-embedding matrix for this
  /// minibatch is a matrix of dimension current_minibatch_.vocab_size by
  /// embedding_mat_.NumRows().  This function sets '*word_embedding' to be a
  /// pointer to the embedding matrix, which will either be '&embedding_mat_'
  /// (in the case where there is no sampling and no sparse feature
  /// representation), or 'word_embedding_storage' otherwise.  In the latter
  /// case, 'word_embedding_storage' will be resized and written to
  /// appropriately.
  void GetWordEmbedding(CuMatrix<BaseFloat> *word_embedding_storage,
                        CuMatrix<BaseFloat> **word_embedding);


  /// This function trains the word-embedding matrix for the minibatch we're
  /// training on (in previous_minibatch_).  'embedding_deriv' is the derivative
  /// w.r.t. the word-embedding for this minibatch (of dimension
  /// previus_minibatch_.vocab_size by embedding_mat_.NumCols()).
  /// You can think of it as the backprop for the function 'GetWordEmbedding()'.
  ///   @param [in] word_embedding_deriv   The derivative w.r.t. the embeddings of
  ///                       just the words used in this minibatch
  ///                       (i.e. the minibatch-level word-embedding matrix,
  ///                       possibly using a subset of words).  This is an input
  ///                       but this function consumes it destructively.
  void TrainWordEmbedding(CuMatrixBase<BaseFloat> *word_embedding_deriv);

  /// This is the function-call that's run as the background thread which
  /// computes the derived parameters for each minibatch.
  void RunBackgroundThread();

  /// This function is invoked by the newly created background thread.
  static void run_background_thread(RnnlmTrainer *trainer) {
    trainer->RunBackgroundThread();
  }

  const RnnlmTrainerOptions &config_;

  // The neural net we are training.
  nnet3::Nnet rnnlm_;

  // Pointer to the object that trains 'rnnlm_'.
  RnnlmCoreTrainer *core_trainer_;

  // The (word or feature) embedding matrix; it's the word embedding matrix if
  // word_feature_mat_.NumRows() == 0, else it's the feature embedding matrix.
  // The dimension is (num-words or num-features) by embedding-dim.
  CuMatrix<BaseFloat> embedding_mat_;


  // Pointer to the object that trains 'embedding_mat_', or NULL if we are not
  // training it.
  RnnlmEmbeddingTrainer *embedding_trainer_;

  // If the --read-sparse-word-features options is provided, then
  // word_feature_mat_ will contain the matrix of sparse word features, of
  // dimension num-words by num-features.  In this case, the word embedding
  // matrix is the product of this matrix times 'embedding_mat_'.
  CuSparseMatrix<BaseFloat> word_feature_mat_;

  // This is the transpose of word_feature_mat_, which is needed only if we
  // train on egs without sampling.  This is only computed once, if and when
  // it's needed.
  CuSparseMatrix<BaseFloat> word_feature_mat_transpose_;




  // num_minibatches_processed_ starts at zero is incremented each time we
  // provide an example to the background thread for computing the derived
  // parameters.
  int32 num_minibatches_processed_;

  // 'current_minibatch' is where the Train() function puts the minibatch that
  // is provided to Train(), so that the background thread can work on it.
  RnnlmExample current_minibatch_;
  // View 'end_of_input_' as part of a unit with current_minibatch_, for threading/access
  // purposes.  It is set by the foreground thread from the destructor, while
  // incrementing the current_minibatch_ready_ semaphore; and when the background
  // thread decrements the semaphore and notices that end_of_input_ is true, it will
  // exit.
  bool end_of_input_;


  // previous_minibatch_ is the previous minibatch that was provided to Train(),
  // but the minibatch that we're currently trainig on.
  RnnlmExample previous_minibatch_;
  // The variables derived_ and active_words_ [and more that I'll add, TODO] are in the same
  // group as previous_minibatch_ from the point of view
  // of threading and access control.
   RnnlmExampleDerived derived_;
  // Only if we are doing subsampling (depends on the eg), active_words_
  // contains the list of active words for the minibatch 'previous_minibatch_';
  // it is a CUDA version of the 'active_words' output by
  // RenumberRnnlmExample().  Otherwise it is empty.
  CuArray<int32> active_words_;
  // Only if we are doing subsampling AND we have sparse word features
  // (i.e. word_feature_mat_ is nonempty), active_word_features_ contains
  // just the rows of word_feature_mat_ which correspond to active_words_.
  // This is a derived quantity computed by the background thread.
  CuSparseMatrix<BaseFloat> active_word_features_;
  // Only if we are doing subsampling AND we have sparse word features,
  // active_word_features_trans_ is the transpose of active_word_features_;
  // This is a derived quantity computed by the background thread.
  CuSparseMatrix<BaseFloat> active_word_features_trans_;


  // The 'previous_minibatch_full_' semaphore is incremented by the background
  // thread once it has written to 'previous_minibatch_' and
  // 'derived_previous_', to let the Train() function know that they are ready
  // to be trained on.  The Train() function waits on this semaphore.
  Semaphore previous_minibatch_full_;

  // The 'previous_minibatch_empty_' semaphore is incremented by the foreground
  // thread when it has done processing previous_minibatch_ and
  // derived_ and active_words_ (and hence, it is safe for the background thread to write
  // to these variables).  The background thread waits on this semaphore once it
  // has finished computing the derived variables; and when it successfully
  // decrements it, it will write to those variables (quickly, via Swap()).
  Semaphore previous_minibatch_empty_;


  // The 'current_minibatch_ready_' semaphore is incremented by the foreground
  // thread from Train(), when it has written the just-provided minibatch to
  // 'current_minibatch_' (it's also incremented by the destructor, together
  // with setting end_of_input_.  The background thread waits on this semaphore
  // before either processing previous_minibatch (if !end_of_input_), or exiting
  // (if end_of_input_).
  Semaphore current_minibatch_full_;

  // The 'current_minibatch_empty_' semaphore is incremented by the background
  // thread when it has done processing current_minibatch_,
  // so, it is safe for the foreground thread to write
  // to this variable).  The foreground thread waits on this semaphore before
  // writing to 'current_minibatch_' (in practice it should get the semaphore
  // immediately since we expect that the foreground thread will have more to
  // do than the background thread).
  Semaphore current_minibatch_empty_;

  std::thread background_thread_;  // Background thread for computing 'derived'
                                   // parameters of a minibatch.


  KALDI_DISALLOW_COPY_AND_ASSIGN(RnnlmTrainer);
};


} // namespace rnnlm
} // namespace kaldi

#endif //KALDI_RNNLM_RNNLM_H_
