/***********************************************************************
 Moses - factored phrase-based language decoder
 Copyright (C) 2009 University of Edinburgh
 
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.
 
 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 ***********************************************************************/

#include <algorithm>
#include <functional>
#include <iostream>
#include <iomanip>
#include <fstream>

#ifdef MPI_ENABLED
#include <mpi.h>
#endif

#include <boost/program_options.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include "AnnealingSchedule.h"
#include "Decoder.h"
#include "Derivation.h"
#include "Gibbler.h"
#include "InputSource.h"
#include "TrainingSource.h"
#include "GibbsOperator.h"
#include "SentenceBleu.h"
#include "GainFunction.h"
#include "CorpusSampler.h"
#include "CorpusSamplerAnnealed.h"
#include "GibblerMaxTransDecoder.h"
#include "MpiDebug.h"
#include "StaticData.h"
#include "Optimizer.h"
#include "SampleAcceptor.h"
#include "TranslationDelta.h"
#include "Utils.h"


using namespace std;
using namespace Josiah;
using namespace Moses;
using boost::lexical_cast;
using boost::bad_lexical_cast;
using boost::split;
using boost::is_any_of;
namespace po = boost::program_options;

/**
 * Main for Josiah - the Gibbs sampler for moses.
 **/
int main(int argc, char** argv) {
  int rank = 0, size = 1;
#ifdef MPI_ENABLED
  MPI_Init(&argc,&argv);
  MPI_Comm comm = MPI_COMM_WORLD;
  MPI_Comm_rank(comm,&rank);
  MPI_Comm_size(comm,&size);
  cerr << "MPI rank: " << rank << endl; 
  cerr << "MPI size: " << size << endl;
#endif
  GibbsTimer timer;
  string stopperConfig;
  unsigned int topn;
  int debug;
  int mpidebug;
  string mpidebugfile;
  int burning_its;
  int mbr_size;
  string inputfile;
  string outputfile;
  string mosesini;
  bool decode;
  bool translate;
  bool translation_distro;
  bool derivation_distro;
  bool help;
  bool expected_cbleu = false;
  bool expected_cbleu_gradient;
  bool expected_cbleu_training;
  unsigned training_batch_size;
  bool mbr_decoding;
  bool do_timing;
  bool show_features;
  int max_training_iterations;
  int num_samples;
  uint32_t seed;
  int lineno;
  bool randomize;
  float scalefactor;
  vector<float> eta;
  float mu;
  string weightfile;
  vector<string> ref_files;
  bool decode_monotone;
  bool decode_zero_weights;
  bool decode_nolm;
  bool decode_random;
  int periodic_decode;
  bool collect_dbyt;
  bool output_max_change;
  bool anneal;
  unsigned int reheatings;
  float max_temp;
  float prior_variance;
  vector<float> prior_mean;
  vector<float> prev_gradient;
  bool expected_cbleu_da;
  float start_temp_expda;
  float stop_temp_expda;  
  float floor_temp_expda;  
  float anneal_ratio_da;
  float gamma;
  bool use_metanormalized_egd;
  int optimizerFreq; 
  float brev_penalty_scaling_factor;
  bool hack_bp_denum;
  int weight_dump_freq;
  string weight_dump_stem;
  int init_iteration_number;
  bool greedy, fixedTemp;
  float fixed_temperature;
  bool collectAll, sampleCtrAll;
  vector<string> ngramorders;
  bool randomScan;
  size_t lag;
  float flip_prob, merge_split_prob, retrans_prob;
  float log_base_factor;
  po::options_description desc("Allowed options");
  desc.add_options()
  ("help",po::value( &help )->zero_tokens()->default_value(false), "Print this help message and exit")
  ("config,f",po::value<string>(&mosesini),"Moses ini file")
  ("verbosity,v", po::value<int>(&debug)->default_value(0), "Verbosity level")
  ("mpi-debug", po::value<int>(&MpiDebug::verbosity)->default_value(0), "Verbosity level for debugging messages used in mpi.")
  ("mpi-debug-file", po::value<string>(&mpidebugfile), "Debug file stem for use by mpi processes")
  ("random-seed,e", po::value<uint32_t>(&seed), "Random seed")
  ("timing,m", po::value(&do_timing)->zero_tokens()->default_value(false), "Display timing information.")
  ("max-samples", po::value<string>(&stopperConfig)->default_value("5"), 
   "How many samples to gather initially (before resampling step)")
  ("samples,s", po::value<int>(&num_samples)->default_value(5), "Number of samples used for training")
  ("burn-in,b", po::value<int>(&burning_its)->default_value(1), "Duration (in sampling iterations) of burn-in period")
  ("scale-factor,c", po::value<float>(&scalefactor)->default_value(1.0), "Scale factor for model weights.")
  ("decode-monotone", po::value(&decode_monotone)->zero_tokens()->default_value(false), "Run the initial decoding monotone.")
  ("decode-nolm", po::value(&decode_nolm)->zero_tokens()->default_value(false), "Run the initial decoding without an lm.")
  ("decode-zero-weights", po::value(&decode_zero_weights)->zero_tokens()->default_value(false), "Run the initial decoding with weights set to zero.")
  ("decode-random", po::value(&decode_random)->zero_tokens()->default_value(false), "Use the random decoder.")
  ("input-file,i",po::value<string>(&inputfile),"Input file containing tokenised source")
  ("output-file-prefix,o",po::value<string>(&outputfile),"Output file prefix for translations, MBR output, etc")
  ("nbest-drv,n",po::value<unsigned int>(&topn)->default_value(0),"Write the top n derivations to stdout")
	("show-features,F",po::value<bool>(&show_features)->zero_tokens()->default_value(false),"Show features and then exit")
	("weights,w",po::value<string>(&weightfile),"Weight file")
  ("decode-derivation,d",po::value( &decode)->zero_tokens()->default_value(false),"Write the most likely derivation to stdout")
  ("decode-translation,t",po::value(&translate)->zero_tokens()->default_value(false),"Write the most likely translation to stdout")
  ("distro-derivation", po::value(&derivation_distro)->zero_tokens()->default_value(false), "Print derivation probability distribution")
  ("distro-translation", po::value(&translation_distro)->zero_tokens()->default_value(false), "Print translation probability distribution")
  ("periodic-derivation,p",po::value(&periodic_decode)->default_value(0), "Periodically write the max derivation to stderr")
  ("max-change", po::value(&output_max_change)->zero_tokens()->default_value(false), "Whenever the max deriv or max trans changes, write it to stderr")
  ("collect-dbyt",po::value(&collect_dbyt)->zero_tokens()->default_value(false), "Collect derivations per translation")
  ("line-number,L", po::value(&lineno)->default_value(0), "Starting reference/line number")
  ("xbleu,x", po::value(&expected_cbleu)->zero_tokens()->default_value(false), "Compute the expected corpus BLEU")
  ("gradient,g", po::value(&expected_cbleu_gradient)->zero_tokens()->default_value(false), "Compute the gradient with respect to expected corpus BLEU")
  ("randomize-batches,R", po::value(&randomize)->zero_tokens()->default_value(false), "Randomize training batches")
  ("gaussian-prior-variance", po::value<float>(&prior_variance)->default_value(0.0f), "Gaussian prior variance (0 for no prior)")
  ("gaussian-prior-mean,P", po::value<vector<float> >(&prior_mean), "Gaussian prior means")
  ("expected-bleu-training,T", po::value(&expected_cbleu_training)->zero_tokens()->default_value(false), "Train to maximize expected corpus BLEU")
  ("max-training-iterations,M", po::value(&max_training_iterations)->default_value(30), "Maximum training iterations")
  ("training-batch-size,S", po::value(&training_batch_size)->default_value(0), "Batch size to use during xpected bleu training, 0 = full corpus")
	("reheatings", po::value<unsigned int>(&reheatings)->default_value(1), "Number of times to reheat the sampler")
	("anneal,a", po::value(&anneal)->default_value(false)->zero_tokens(), "Use annealing during the burn in period")
	("max-temp", po::value<float>(&max_temp)->default_value(4.0), "Annealing maximum temperature")
  ("eta", po::value<vector<float> >(&eta), "Default learning rate for SGD/EGD")
  ("prev-gradient", po::value<vector<float> >(&prev_gradient), "Previous gradient for restarting SGD/EGD")
  ("mu", po::value<float>(&mu)->default_value(1.0f), "Metalearning rate for EGD")
  ("gamma", po::value<float>(&gamma)->default_value(0.9f), "Smoothing parameter for Metanormalized EGD ")
  ("ref,r", po::value<vector<string> >(&ref_files), "Reference translation files for training")
  ("extra-feature-config,X", po::value<string>(), "Configuration file for extra (non-Moses) features")
  ("use-metanormalized-egd,N", po::value(&use_metanormalized_egd)->zero_tokens()->default_value(false), "Use metanormalized EGD")
  ("expected-bleu-deterministic-annealing-training,D", po::value(&expected_cbleu_da)->zero_tokens()->default_value(false), "Train to maximize expected corpus BLEU using deterministic annealing")   
  ("optimizer-freq", po::value<int>(&optimizerFreq)->default_value(1),"Number of optimization to perform at given temperature")
  ("initial-det-anneal-temp", po::value<float>(&start_temp_expda)->default_value(1000.0f), "Initial deterministic annealing entropy temperature")
  ("final-det-anneal-temp", po::value<float>(&stop_temp_expda)->default_value(0.001f), "Final deterministic annealing entropy temperature")
  ("floor-temp", po::value<float>(&floor_temp_expda)->default_value(0.0f), "Floor temperature for det annealing")
  ("det-annealing-ratio,A", po::value<float>(&anneal_ratio_da)->default_value(0.5f), "Deterministc annealing ratio")
  ("hack-bp-denum,H", po::value(&hack_bp_denum)->default_value(false), "Use a predefined scalar as denum in BP computation")
  ("bp-scale,B", po::value<float>(&brev_penalty_scaling_factor)->default_value(1.0f), "Scaling factor for sent level brevity penalty for BLEU - default is 1.0")
  ("weight-dump-freq", po::value<int>(&weight_dump_freq)->default_value(0), "Frequency to dump weight files during training")
  ("weight-dump-stem", po::value<string>(&weight_dump_stem)->default_value("weights"), "Stem of filename to use for dumping weights")
      ("init-iteration-number", po::value<int>(&init_iteration_number)->default_value(0), "First training iteration will be one after this (useful for restarting)")
  ("greedy", po::value(&greedy)->zero_tokens()->default_value(false), "Greedy sample acceptor")
  ("fixed-temp-accept", po::value(&fixedTemp)->zero_tokens()->default_value(false), "Fixed temperature sample acceptor")
  ("fixed-temperature", po::value<float>(&fixed_temperature)->default_value(1.0f), "Temperature for fixed temp sample acceptor")
  ("collect-all", po::value(&collectAll)->zero_tokens()->default_value(false), "Collect all samples generated")
  ("sample-ctr-all", po::value(&sampleCtrAll)->zero_tokens()->default_value(false), "When in CollectAllSamples model, increment collection ctr after each sample has been collected")
  ("mh.ngramorders", po::value< vector <string> >(&ngramorders), "Indicate LMs and ngram orders to be used during MH/Gibbs")
  ("random-scan", po::value(&randomScan)->zero_tokens()->default_value(false), "Sample using random scan")
  ("lag", po::value<size_t>(&lag)->default_value(10), "Lag between collecting samples")
  ("flip-prob", po::value<float>(&flip_prob)->default_value(0.6f), "Probability of applying flip operator during random scan")
  ("merge-split-prob", po::value<float>(&merge_split_prob)->default_value(0.2f), "Probability of applying merge-split operator during random scan")
  ("retrans-prob", po::value<float>(&retrans_prob)->default_value(0.2f), "Probability of applying retrans operator during random scan")
    ("log-base-factor", po::value<float>(&log_base_factor)->default_value(1.0f), "Scaling factor for log probabilities in translation and language models");
  
  
  po::options_description cmdline_options;
  cmdline_options.add(desc);
  po::variables_map vm;
  po::store(po::command_line_parser(argc,argv).
            options(cmdline_options).run(), vm);
  po::notify(vm);
  
  feature_vector extra_features; 
  if (!vm["extra-feature-config"].empty()){
    configure_features_from_file(vm["extra-feature-config"].as<std::string>(), extra_features);
  }
  std::cerr << "Using " << extra_features.size() << " extra features" << std::endl;
  
  if (help) {
    std::cout << "Usage: " + string(argv[0]) +  " -f mosesini-file [options]" << std::endl;
    std::cout << desc << std::endl;
    return 0;
  }
  
  if (expected_cbleu_training && expected_cbleu_da) {
    std::cerr << "Incorrect usage: Cannot do both expected bleu training and expected bleu deterministic annealing training" << std::endl;
    return 0;
  }
  
  if (randomScan) {
    float opProb = flip_prob + merge_split_prob + retrans_prob;
    if (fabs(1.0 - opProb) > 0.00001) {
      std::cerr << "Incorrect usage: specified operator probs should sum up to 1" << std::endl;
      return 0;  
    }
  }
  
  if (translation_distro) translate = true;
  if (derivation_distro) decode = true;
  bool defaultCtrIncrementer = !sampleCtrAll;
  
  expected_cbleu = false;
  if (expected_cbleu_gradient == true && expected_cbleu_da == false) expected_cbleu = true;
  
  if (mosesini.empty()) {
    cerr << "Error: No moses ini file specified" << endl;
    return 1;
  }
  
  if (mpidebugfile.length()) {
    MpiDebug::init(mpidebugfile,rank);
  }
  cerr << "optimizer freq " << optimizerFreq << endl;
  assert(optimizerFreq != 0);
  
  if (do_timing) {
    timer.on();
  }

  
  if (log_base_factor != 1.0) {
      cerr << "Setting log base factor to " << log_base_factor << endl;
      SetLogBaseFactor(log_base_factor);
  }
 
 
  //set up moses
  initMoses(mosesini,weightfile,debug);
  auto_ptr<Decoder> decoder;
  if (decode_random) {
    if (decode_monotone || decode_zero_weights || decode_nolm) {
      cerr << "Error:: Random decoder cannot be used with any other options." << endl;
#ifdef MPI_ENABLED
      MPI_Finalize();
#endif
      return -1;
    }
    decoder.reset(new RandomDecoder());
  } else {
    decoder.reset(new MosesDecoder());
  }
  
  
  auto_ptr<MHAcceptor> mhAcceptor;
  bool doMH = false;
  
  
  //Initialise LMs info
  map <LanguageModel*, int> proposalLMInfo, targetLMInfo;
  
  const LMList& languageModels = StaticData::Instance().GetAllLM();
  for (LMList::const_iterator i = languageModels.begin(); i != languageModels.end(); ++i) {  
    LanguageModel* lm = *i; 
    targetLMInfo[lm] = lm->GetNGramOrder();
  }
  proposalLMInfo = targetLMInfo;
  if (ngramorders.size()) { //MH info
    mhAcceptor.reset(new MHAcceptor());
    mhAcceptor.get()->setTargetLMInfo(targetLMInfo);
    bool success = false;
    for (size_t i = 0; i < ngramorders.size(); ++i) {
      vector <string> tokens = Tokenize(ngramorders[i], ":");
      assert (tokens.size() == 2);
      string name = tokens[0];
      int order = atoi(tokens[1].c_str());
      LanguageModel* lm;
      success = ValidateAndGetLMFromName(name, lm);
      if (success) {
        doMH = true;
        proposalLMInfo[lm] = order;
        mhAcceptor.get()->addScoreProducer(lm); 
      }
      else {
        cerr << "Error: That feature does not exist. " << endl;
#ifdef MPI_ENABLED
        MPI_Finalize();
#endif
        return -1;
      }
    }
    if (success) {
      mhAcceptor.get()->setProposalLMInfo(proposalLMInfo);  
    } 
  }
  
  // may be invoked just to get a features list
  if (show_features) {
    OutputWeights(cout);
#ifdef MPI_ENABLED
    MPI_Finalize();
#endif
    return 0;
  }
  
  
  
  if (decode_monotone) {
    decoder->SetMonotone(true);
  }
  
  if (decode_zero_weights) {
    decoder->SetZeroWeights(true);
  }
  
  if (decode_nolm) {
    decoder->SetNoLM(true);
  }
  
  //scale model weights
  vector<float> weights = StaticData::Instance().GetAllWeights();
  transform(weights.begin(),weights.end(),weights.begin(),bind2nd(multiplies<float>(),scalefactor));
  const_cast<StaticData&>(StaticData::Instance()).SetAllWeights(weights);
  VERBOSE(1,"Scaled weights by factor of " << scalefactor << endl);
  
  
  
  if (vm.count("random-seed")) {
    RandomNumberGenerator::instance().setSeed(seed + rank);
  }      
  
  GainFunctionVector g;
  if (ref_files.size() > 0) LoadReferences(ref_files, inputfile, &g, brev_penalty_scaling_factor, hack_bp_denum);
  
  ostream* out = &cout;
  if (!outputfile.empty()) {
    ostringstream os;
    os << setfill('0');
    os << outputfile << '.' << setw(3) << rank << "_of_" << size;
    VERBOSE(1, "Writing output to: " << os.str() << endl);
    out = new ofstream(os.str().c_str());
  }
  auto_ptr<istream> in;
  auto_ptr<InputSource> input;
  
  auto_ptr<Optimizer> optimizer;
  
  eta.resize(weights.size());   
  
  
  
  prev_gradient.resize(weights.size());
  
  if (use_metanormalized_egd) {
    optimizer.reset(new MetaNormalizedExponentiatedGradientDescent(
                                                                   ScoreComponentCollection(eta),
                                                                   mu,
                                                                   0.1f,   // minimal step scaling factor
                                                                   gamma,                                       
                                                                   max_training_iterations,
                                                                   ScoreComponentCollection(prev_gradient)));
  } else {
    optimizer.reset(new ExponentiatedGradientDescent(
                                                     ScoreComponentCollection(eta),
                                                     mu,
                                                     0.1f,   // minimal step scaling factor
                                                     max_training_iterations,
                                                     ScoreComponentCollection(prev_gradient)));
  }
  if (optimizer.get()) {
      optimizer->SetIteration(init_iteration_number);
  }
  if (prior_variance != 0.0f) {
    assert(prior_variance > 0);
    std::cerr << "Using Gaussian prior: \\sigma^2=" << prior_variance << endl;
    for (size_t i = 0; i < prior_mean.size(); ++i)
      std::cerr << "  \\mu_" << i << " = " << prior_mean[i] << endl;
    optimizer->SetUseGaussianPrior(prior_mean, prior_variance);
  }
  ExpectedBleuTrainer* trainer = NULL;
  vector<string> input_lines;
  ifstream infiles(inputfile.c_str());
  assert (infiles);
  while(infiles) {
    string line;
    getline(infiles, line);
    if (line.empty() && infiles.eof()) break;
    assert(!line.empty());
    input_lines.push_back(line);
  }
  VERBOSE(1, "Loaded " << input_lines.size() << " lines in training mode" << endl);
  if (!training_batch_size || training_batch_size > input_lines.size())
    training_batch_size = input_lines.size();
  VERBOSE(1, "Batch size: " << training_batch_size << endl);
  trainer = new ExpectedBleuTrainer(rank, size, training_batch_size, &input_lines, seed, randomize, optimizer.get(),     
                                      init_iteration_number,weight_dump_freq, weight_dump_stem);

  input.reset(trainer);
  
  auto_ptr<AnnealingSchedule> annealingSchedule;
  if (anneal) {
    annealingSchedule.reset(new LinearAnnealingSchedule(burning_its, max_temp));  
  }
  
  auto_ptr<AnnealingSchedule> detAnnealingSchedule;
  if (expected_cbleu_da) {
    detAnnealingSchedule.reset(new ExponentialAnnealingSchedule(start_temp_expda, stop_temp_expda, floor_temp_expda, anneal_ratio_da));
  }
  
  
  auto_ptr<CorpusSamplerCollector> elCollector;
  Sampler sampler;
  //configure the sampler
  sampler.SetAnnealingSchedule(annealingSchedule.get());
  VERBOSE(2,"Reheatings: " << reheatings << endl);
  sampler.SetReheatings(reheatings);
  sampler.SetRandomScan(randomScan); //random or sequential scan
  sampler.SetLag(lag); //thinning factor for sample collection
  MergeSplitOperator mso(randomScan, merge_split_prob);
  FlipOperator fo(randomScan, flip_prob);
  TranslationSwapOperator tso(randomScan, retrans_prob);
    mso.setGibbsLMInfo(targetLMInfo);
    fo.setGibbsLMInfo(targetLMInfo);
    tso.setGibbsLMInfo(targetLMInfo);
  
    if (proposalLMInfo.size()) {
      mso.setGibbsLMInfo(proposalLMInfo);
      mso.addMHAcceptor(mhAcceptor.get());
      tso.setGibbsLMInfo(proposalLMInfo);
      tso.addMHAcceptor(mhAcceptor.get()); 
    }

  sampler.AddOperator(&mso);
  sampler.AddOperator(&tso);
  sampler.AddOperator(&fo);
  
  //Acceptor
  auto_ptr<SampleAcceptor> acceptor;
  if (greedy || fixed_temperature == 0) {
    acceptor.reset(new GreedyAcceptor());
  }
  else if (fixedTemp){
    acceptor.reset(new FixedTempAcceptor(fixed_temperature));
  }
  else {
    acceptor.reset(new RegularAcceptor);
  }
  
  
  //sampler stopping strategy; TODO: push parsing of config into StoppingStrategy ctor ?
  auto_ptr<StopStrategy> stopper;
  size_t iterations;
  try {
    iterations = lexical_cast<size_t>(stopperConfig);
    if (iterations < num_samples)
      iterations = num_samples;
    stopper.reset(new CountStopStrategy(iterations)); 
  } catch (bad_lexical_cast&) {/* do nothing*/}
  
  if (!stopper.get()) {
    cerr << "Error: unable to parse stopper config string '" << stopperConfig << "'" << endl;
    exit(1);
  }
  
  sampler.SetStopper(stopper.get());
  sampler.SetBurnIn(burning_its);
  
  if (expected_cbleu) {
    elCollector.reset(new CorpusSamplerCollector(num_samples, sampler));
    sampler.AddCollector(elCollector.get());
  }
  else if (expected_cbleu_da) {
    elCollector.reset(new CorpusSamplerAnnealedCollector(num_samples, sampler));
    sampler.AddCollector(elCollector.get());
  }
  
  timer.check("Processing input file");
  int sentCtr = 0;
  while (input->HasMore()) {
    string line;
    input->GetSentence(&line, &lineno);
    if (line.empty()) {
      if (!input->HasMore()) continue;
      assert(!"I don't like empty lines");
    }
    
    elCollector->addGainFunction(&(g[lineno]));
    //Set the annealing temperature
    if (expected_cbleu_da) {
      int it = optimizer->GetIteration() / optimizerFreq  ;
      float temp = detAnnealingSchedule->GetTemperatureAtTime(it);
      
      CorpusSamplerAnnealedCollector* annealedELCollector = static_cast<CorpusSamplerAnnealedCollector*>(elCollector.get());
      annealedELCollector->SetTemperature(temp);
      cerr << "Annealing temperature " << annealedELCollector->GetTemperature() << endl;
      
      
      
      
    }
    
    Hypothesis* hypothesis;
    TranslationOptionCollection* toc;
    
    timer.check("Running decoder");
    
    
    std::vector<Word> source;
    decoder->decode(line,hypothesis,toc,source);
    timer.check("Running sampler");
    
    TranslationDelta::lmcalls = 0;
    if (doMH) {
      MHAcceptor::mhtotal = 0;  
      MHAcceptor::acceptanceCtr = 0;  
    }
    sampler.Run(hypothesis,toc,source,extra_features, acceptor.get(), collectAll, defaultCtrIncrementer);  
    if (doMH) {
      VERBOSE(1, "Total number of Metropolis-Hastings Steps :" << MHAcceptor::mhtotal << endl) 
      VERBOSE(1, "Total number of accepted Metropolis-Hastings Steps :" <<  MHAcceptor::acceptanceCtr << endl) 
    }
    
    VERBOSE(1, "Language model calls: " << TranslationDelta::lmcalls << endl);
    timer.check("Outputting results");
    
    //Now resample
    elCollector->resample(sentCtr);
    
    //cerr << "curr " << trainer->GetCurr() << ", end  " << trainer->GetCurrEnd() << endl; 
    if (trainer && trainer->GetCurr() == trainer->GetCurrEnd()) {//Now need to aggregate the feature vectors and bleu stats
#ifdef MPI_ENABLED  
      elCollector->AggregateSamples(rank);    
#endif      
      ScoreComponentCollection gradient;
      float exp_trans_len = 0;
      float unreg_exp_gain = 0;
      float exp_gain = 0;

#ifdef MPI_ENABLED  
      if (rank == 0) {
        exp_gain = elCollector->UpdateGradient(&gradient, &exp_trans_len, &unreg_exp_gain);  
      }
#else
      exp_gain = elCollector->UpdateGradient(&gradient, &exp_trans_len, &unreg_exp_gain);
#endif
      if (trainer)
        
        trainer->IncorporateCorpusGradient(
                                           exp_trans_len,
                                           elCollector->getReferenceLength(),
                                           exp_gain,
                                           unreg_exp_gain,
                                           gradient,
                                           decoder.get());
      elCollector->reset();
    }

    ++lineno;
    ++sentCtr;
  }
      
#ifdef MPI_ENABLED
  MPI_Finalize();
#endif
  (*out) << flush;
  if (!outputfile.empty())
    delete out;
  return 0;
}