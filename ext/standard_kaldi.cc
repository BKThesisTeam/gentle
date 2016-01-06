// Escape hatch from Kaldi.
//
// Use standard in/out trickery to get control flow into other languages.

// Based on online2-wav-nnet2-latgen-faster

#include <stdlib.h>

#include "online2/online-nnet2-decoding.h"
#include "online2/onlinebin-util.h"
#include "online2/online-timing.h"
#include "online2/online-endpoint.h"
#include "fstext/fstext-lib.h"
#include "lat/lattice-functions.h"
#include "lat/word-align-lattice.h"
#include "hmm/hmm-utils.h"
#include "thread/kaldi-thread.h"

const int arate = 8000;

// Phonemes contain information about phoneme timing in the
// transcript. They're included in AlignedWords.
struct Phoneme {
  std::string token;
  double duration;
  bool has_duration;
};

// AlignedWord contains information about the timing of a
// word heard in an audio file.
struct AlignedWord {
  std::string token;
  double start;
  bool has_start;
  double duration;
  bool has_duration;
  std::vector<Phoneme> phones;
};

// A Hypothesis is a possible transcription of an auio file.
// It's the output of this program.
typedef std::vector<AlignedWord> Hypothesis;

// Hypothesizer reads Kaldi lattices and extracts Hypothesis structs
// with possible transcriptions of the utterances they contain.
class Hypothesizer {
 public:
  Hypothesizer(float frame_shift,
               const kaldi::TransitionModel& transition_model,
               const kaldi::WordBoundaryInfo& word_boundary_info,
               const fst::SymbolTable* word_syms,
               const fst::SymbolTable* phone_syms)
      : frame_shift_(frame_shift),
        transition_model_(&transition_model),
        word_boundary_info_(&word_boundary_info),
        word_syms_(word_syms),
        phone_syms_(phone_syms) {}

  // GetPartial only generates the word sequence for the best
  // hypothesis in the lattice. It doesn't word-align the transcript
  // or anything fancy. It's fast and good for partial results.
  Hypothesis GetPartial(const kaldi::Lattice& lattice);

  // GetFull extracts the best hypothesis in the lattice, pulling
  // out all the stops. It word aligns, phoneme aligns, and generates
  // confidences. It's slower and should only be used when it matters.
  Hypothesis GetFull(const kaldi::Lattice& lattice);

 private:
  float frame_shift_;
  const kaldi::TransitionModel* transition_model_;
  const kaldi::WordBoundaryInfo* word_boundary_info_;
  const fst::SymbolTable* word_syms_;
  const fst::SymbolTable* phone_syms_;
};

Hypothesis Hypothesizer::GetPartial(const kaldi::Lattice& lattice) {
  Hypothesis hyp;

  // Let's see what words are in here..
  std::vector<int32> words;
  std::vector<int32> alignment;
  kaldi::LatticeWeight weight;
  GetLinearSymbolSequence(lattice, &alignment, &words, &weight);

  for (int32 word : words) {
    AlignedWord aligned;
    aligned.token = this->word_syms_->Find(word);
    hyp.push_back(aligned);
  }

  return hyp;
}

Hypothesis Hypothesizer::GetFull(const kaldi::Lattice& lattice) {
  Hypothesis hyp;

  kaldi::CompactLattice clat;
  ConvertLattice(lattice, &clat);

  // Compute prons alignment (see: kaldi/latbin/nbest-to-prons.cc)
  kaldi::CompactLattice aligned_clat;

  std::vector<int32> words, times, lengths;
  std::vector<std::vector<int32> > prons;
  std::vector<std::vector<int32> > phone_lengths;

  WordAlignLattice(clat, *this->transition_model_, *this->word_boundary_info_,
                   0, &aligned_clat);

  CompactLatticeToWordProns(*this->transition_model_, clat, &words, &times,
                            &lengths, &prons, &phone_lengths);

  for (int i = 0; i < words.size(); i++) {
    AlignedWord aligned;
    aligned.token = this->word_syms_->Find(words[i]);
    aligned.start = times[i] * this->frame_shift_;
    aligned.has_start = true;
    aligned.duration = lengths[i] * this->frame_shift_;
    aligned.has_duration = true;

    for (size_t j = 0; j < phone_lengths[i].size(); j++) {
      Phoneme phone;
      phone.token = this->phone_syms_->Find(prons[i][j]);
      phone.duration = phone_lengths[i][j] * this->frame_shift_;
      phone.has_duration = true;
      aligned.phones.push_back(phone);
    }

    hyp.push_back(aligned);
  }

  return hyp;
}

// Decoder represents an in-progress transcription of an utterance.
class Decoder {
 public:
  Decoder(const kaldi::OnlineNnet2FeaturePipelineInfo& info,
          const kaldi::TransitionModel& transition_model,
          const kaldi::OnlineNnet2DecodingConfig& nnet2_decoding_config,
          const kaldi::nnet2::AmNnet& nnet,
          const fst::Fst<fst::StdArc>* decode_fst,
          const kaldi::OnlineIvectorExtractorAdaptationState& adaptation_state);

  // AddChunk adds an audio chunk of audio to the decoding pipeline.
  void AddChunk(kaldi::BaseFloat sampling_rate,
                const kaldi::VectorBase<kaldi::BaseFloat>& waveform);
  // GetAdaptationState gets the ivector extractor's adaptation state
  void GetAdaptationState(
      kaldi::OnlineIvectorExtractorAdaptationState* adaptation_state);
  // GetBestPath outputs the decoder's current one-best lattice.
  kaldi::Lattice GetBestPath();
  // Finalize is called when you're finished adding chunks. It flushes the
  // data pipeline and cleans up the lattice. After calling Finalize all
  // calls to AddChunk will fail.
  void Finalize();

 private:
  // AdvanceDecoding processes any chunks in the decoding pipeline,
  // applying silence weighting.
  void AdvanceDecoding();

  kaldi::OnlineNnet2FeaturePipeline feature_pipeline_;
  kaldi::SingleUtteranceNnet2Decoder decoder_;

  kaldi::OnlineSilenceWeighting silence_weighting_;
  std::vector<std::pair<int32, kaldi::BaseFloat> > delta_weights_;

  bool finalized_;
};

Decoder::Decoder(
    const kaldi::OnlineNnet2FeaturePipelineInfo& info,
    const kaldi::TransitionModel& transition_model,
    const kaldi::OnlineNnet2DecodingConfig& nnet2_decoding_config,
    const kaldi::nnet2::AmNnet& nnet,
    const fst::Fst<fst::StdArc>* decode_fst,
    const kaldi::OnlineIvectorExtractorAdaptationState& adaptation_state)
    : feature_pipeline_(info),
      decoder_(nnet2_decoding_config,
               transition_model,
               nnet,
               *decode_fst,
               &feature_pipeline_),
      silence_weighting_(transition_model, info.silence_weighting_config),
      finalized_(false) {
  this->feature_pipeline_.SetAdaptationState(adaptation_state);
}

void Decoder::AddChunk(kaldi::BaseFloat sampling_rate,
                       const kaldi::VectorBase<kaldi::BaseFloat>& waveform) {
  if (finalized_) {
    return;
  }

  this->feature_pipeline_.AcceptWaveform(sampling_rate, waveform);
  this->AdvanceDecoding();
}

void Decoder::GetAdaptationState(
    kaldi::OnlineIvectorExtractorAdaptationState* adaptation_state) {
  this->feature_pipeline_.GetAdaptationState(adaptation_state);
}

void Decoder::AdvanceDecoding() {
  // Down-weight silence in ivector estimation
  if (this->silence_weighting_.Active()) {
    this->silence_weighting_.ComputeCurrentTraceback(this->decoder_.Decoder());
    this->silence_weighting_.GetDeltaWeights(
        this->feature_pipeline_.NumFramesReady(), &this->delta_weights_);
    this->feature_pipeline_.UpdateFrameWeights(this->delta_weights_);
  }

  this->decoder_.AdvanceDecoding();
}

kaldi::Lattice Decoder::GetBestPath() {
  kaldi::Lattice lattice;

  if (this->decoder_.NumFramesDecoded() == 0) {
    return lattice;
  }

  this->decoder_.GetBestPath(this->finalized_, &lattice);

  return lattice;
}

void Decoder::Finalize() {
  this->finalized_ = true;
  this->feature_pipeline_.InputFinished();
  this->AdvanceDecoding();
  this->decoder_.FinalizeDecoding();
}

// MarshalPhones serializes a list of phonemes as JSON
std::string MarshalPhones(const vector<Phoneme>& phones) {
  std::stringstream ss;

  ss << "[";
  for (int i = 0; i < phones.size(); i++) {
    Phoneme phone = phones[i];
    ss << "{" << std::endl;

    ss << "\"phone\":\"" << phone.token << "\",";
    if (phone.has_duration) {
      ss << "\"duration\":" << phone.duration;
    }

    ss << "}";
    if (i < phones.size() - 1) {
      ss << ",";
    }
  }
  ss << "]";

  return ss.str();
}

// MarshalHypothesis serializes a Hypothesis struct as JSON
std::string MarshalHypothesis(const Hypothesis& hypothesis) {
  std::stringstream ss;

  Hypothesis no_eps;
  for (AlignedWord word : hypothesis) {
    if (word.token == "<eps>") {
      // Don't output anything for <eps> links, which correspond to silence....
      continue;
    }
    no_eps.push_back(word);
  }

  ss << "{\"hypothesis\":";
  ss << "[";

  for (int i = 0; i < no_eps.size(); i++) {
    AlignedWord word = no_eps[i];

    ss << "{";

    ss << "\"word\":\"" << word.token << "\",";
    if (word.has_start) {
      ss << "\"start\":" << word.start << ",";
    }
    if (word.has_duration) {
      ss << "\"duration\":" << word.duration << ",";
    }
    ss << "\"phones\":" << MarshalPhones(word.phones);

    ss << "}";
    if (i < no_eps.size() - 1) {
      ss << ",";
    }
  }

  ss << "]";
  ss << "}" << std::endl;

  return ss.str();
}

void ConfigFeatureInfo(kaldi::OnlineNnet2FeaturePipelineInfo& info,
                       std::string ivector_model_dir) {
  // online_nnet2_decoding.conf
  info.feature_type = "mfcc";

  // ivector_extractor.conf
  info.use_ivectors = true;
  ReadKaldiObject(ivector_model_dir + "/final.mat",
                  &info.ivector_extractor_info.lda_mat);
  ReadKaldiObject(ivector_model_dir + "/global_cmvn.stats",
                  &info.ivector_extractor_info.global_cmvn_stats);
  ReadKaldiObject(ivector_model_dir + "/final.dubm",
                  &info.ivector_extractor_info.diag_ubm);
  ReadKaldiObject(ivector_model_dir + "/final.ie",
                  &info.ivector_extractor_info.extractor);
  info.ivector_extractor_info.greedy_ivector_extractor = true;
  info.ivector_extractor_info.ivector_period = 10;
  info.ivector_extractor_info.max_count = 0.0;
  info.ivector_extractor_info.max_remembered_frames = 1000;
  info.ivector_extractor_info.min_post = 0.025;
  info.ivector_extractor_info.num_cg_iters = 15;
  info.ivector_extractor_info.num_gselect = 5;
  info.ivector_extractor_info.posterior_scale = 0.1;
  info.ivector_extractor_info.use_most_recent_ivector = true;

  // splice.conf
  info.ivector_extractor_info.splice_opts.left_context = 3;
  info.ivector_extractor_info.splice_opts.right_context = 3;

  // mfcc.conf
  info.mfcc_opts.frame_opts.samp_freq = arate;
  info.mfcc_opts.use_energy = false;

  info.ivector_extractor_info.Check();
}

void ConfigDecoding(kaldi::OnlineNnet2DecodingConfig& config) {
  config.decodable_opts.acoustic_scale = 0.1;
  config.decoder_opts.lattice_beam = 6.0;
  config.decoder_opts.beam = 15.0;
  config.decoder_opts.max_active = 7000;
}

void ConfigEndpoint(kaldi::OnlineEndpointConfig& config) {
  config.silence_phones = "1:2:3:4:5:6:7:8:9:10:11:12:13:14:15:16:17:18:19:20";
}

void usage() {
  fprintf(stderr, "usage: standard_kaldi nnet_dir hclg_path proto_lang_dir\n");
}

// The status codes used by the RPC. Maps to HTTP status codes.
enum {
  STATUS_OK = 200,
  STATUS_BAD_REQUEST = 400,
  STATUS_INTERNAL_SERVER_ERROR = 500
} RPCStatus;

// Parse the method part of an RPC request. Returns false if the data is
// malformed.
//
// Methods have this form:
//   METHOD <ARG1> <ARG2> ... <ARGN>\n
bool RPCReadMethod(std::istream& stream, string* method, vector<string>* args) {
  std::string line;
  if (!std::getline(stream, line)) {
    return false;
  }
  std::stringstream ss(line);

  if (!(ss >> *method)) {
    return false;
  }

  string buf;
  while (ss >> buf) {
    args->push_back(buf);
  }

  return true;
}

// Parse the body part of an RPC request. Returns false if the data is
// malformed.
//
// Bodies have this form:
//   BODY_SIZE\n
//   BODY\n
bool RPCReadBody(std::istream& stream, vector<char>* body) {
  string line;
  if (!std::getline(stream, line)) {
    return false;
  }
  std::stringstream ss(line);

  size_t body_size;
  if (!(ss >> body_size)) {
    return false;
  }

  body->resize(body_size);
  if (!stream.read(&body->front(), body_size)) {
    return false;
  }

  char trailing_newline;
  if (!stream.get(trailing_newline)) {
    return false;
  }

  return true;
}

// Write the reply part of the RPC.
//
// Replies have this form:
//   STATUS\n
//   BODY_SIZE\n
//   BODY\n
void RPCWriteReply(std::ostream& stream,
                   const int& status,
                   const vector<char>& body) {
  stream << status << std::endl;
  stream << body.size() << std::endl;
  stream.write(&body[0], body.size());
  stream << std::endl;
}

// Write the reply part of the RPC. Same as the above but accepts strings.
void RPCWriteReply(std::ostream& stream,
                   const int& status,
                   const string& body_str) {
  vector<char> body(body_str.begin(), body_str.end());
  RPCWriteReply(stream, status, body);
}

int main(int argc, char* argv[]) {
  using namespace kaldi;
  using namespace fst;

  if (argc != 4) {
    usage();
    return EXIT_FAILURE;
  }

  const string nnet_dir = argv[1];
  const string fst_rxfilename = argv[2];
  const string proto_lang_dir = argv[3];

  const string ivector_model_dir = nnet_dir + "/ivector_extractor";
  const string nnet2_rxfilename = proto_lang_dir + "/modeldir/final.mdl";
  const string word_syms_rxfilename = proto_lang_dir + "/langdir/words.txt";
  const string phone_syms_rxfilename = proto_lang_dir + "/langdir/phones.txt";
  const string word_boundary_filename =
      proto_lang_dir + "/langdir/phones/word_boundary.int";

  setbuf(stdout, NULL);

  std::cerr << "Loading...\n";

  OnlineNnet2FeaturePipelineInfo feature_info;
  ConfigFeatureInfo(feature_info, ivector_model_dir);
  OnlineNnet2DecodingConfig nnet2_decoding_config;
  ConfigDecoding(nnet2_decoding_config);
  OnlineEndpointConfig endpoint_config;
  ConfigEndpoint(endpoint_config);

  WordBoundaryInfoNewOpts opts;  // use default opts
  WordBoundaryInfo word_boundary_info(opts, word_boundary_filename);

  BaseFloat frame_shift = feature_info.FrameShiftInSeconds();
  fprintf(stderr, "Frame shift is %f secs.\n", frame_shift);

  TransitionModel trans_model;

  nnet2::AmNnet nnet;
  {
    bool binary;
    Input ki(nnet2_rxfilename, &binary);
    trans_model.Read(ki.Stream(), binary);
    nnet.Read(ki.Stream(), binary);
  }

  // This one is much slower than the others.
  fst::Fst<fst::StdArc>* decode_fst = ReadFstKaldi(fst_rxfilename);

  fst::SymbolTable* word_syms =
      fst::SymbolTable::ReadText(word_syms_rxfilename);
  fst::SymbolTable* phone_syms =
      fst::SymbolTable::ReadText(phone_syms_rxfilename);

  OnlineSilenceWeighting silence_weighting(
      trans_model, feature_info.silence_weighting_config);

  Hypothesizer hypothesizer(frame_shift, trans_model, word_boundary_info,
                            word_syms, phone_syms);

  kaldi::OnlineIvectorExtractorAdaptationState adaptation_state(
      feature_info.ivector_extractor_info);

  std::unique_ptr<Decoder> decoder(new Decoder(feature_info, trans_model,
                                               nnet2_decoding_config, nnet,
                                               decode_fst, adaptation_state));

  std::ostream& out_stream = std::cout;
  std::istream& in_stream = std::cin;

  RPCWriteReply(out_stream, STATUS_OK, "loaded");

  while (!in_stream.eof()) {
    string method;
    vector<string> args;
    vector<char> body;

    if (!RPCReadMethod(in_stream, &method, &args)) {
      RPCWriteReply(out_stream, STATUS_BAD_REQUEST,
                    "malformed method '" + method + "'");
      continue;
    }
    if (!RPCReadBody(in_stream, &body)) {
      RPCWriteReply(out_stream, STATUS_BAD_REQUEST, "malformed body");
      continue;
    }

    try {
      if (method == "stop") {
        RPCWriteReply(out_stream, STATUS_OK, "goodbye");
        return 0;

      } else if (method == "reset") {
        // Reset all decoding state.
        decoder.reset(new Decoder(feature_info, trans_model,
                                  nnet2_decoding_config, nnet, decode_fst,
                                  adaptation_state));
        RPCWriteReply(out_stream, STATUS_OK, "");

      } else if (method == "push-chunk") {
        // Add a chunk of audio to the decoding pipeline.
        int sample_count = body.size() / 2;

        Vector<BaseFloat> wave_part(sample_count);
        for (int i = 0; i < sample_count; i++) {
          int16_t sample = *reinterpret_cast<int16_t*>(&body[i * 2]);
          wave_part(i) = sample;
        }

        decoder->AddChunk(arate, wave_part);
        RPCWriteReply(out_stream, STATUS_OK, "");

      } else if (method == "get-partial") {
        // Dump the provisional (non-word-aligned) transcript for the current
        // lattice.

        kaldi::Lattice partial_lat = decoder->GetBestPath();
        Hypothesis partial = hypothesizer.GetPartial(partial_lat);
        string serialized = MarshalHypothesis(partial);

        RPCWriteReply(out_stream, STATUS_OK, serialized);

      } else if (method == "get-final") {
        // Dump the final, phone-aligned transcript for the current lattice.
        kaldi::Lattice final_lat = decoder->GetBestPath();
        Hypothesis final = hypothesizer.GetFull(final_lat);
        string serialized = MarshalHypothesis(final);

        RPCWriteReply(out_stream, STATUS_OK, serialized);

      } else {
        RPCWriteReply(out_stream, STATUS_BAD_REQUEST, "unknown method");
        continue;
      }

    } catch (const std::exception& e) {
      RPCWriteReply(out_stream, STATUS_INTERNAL_SERVER_ERROR, e.what());
      continue;
    }
  }

  return 0;
}
