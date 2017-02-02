#include <algorithm>
#include <chrono>
#include <iomanip>
#include <string>
#include <cstdio>
#include <boost/timer/timer.hpp>
#include <boost/chrono.hpp>
#include <boost/program_options.hpp>
#include <thread>
#include <chrono>
#include <mutex>

#include "marian.h"
#include "optimizers/optimizers.h"
#include "optimizers/clippers.h"
#include "data/batch_generator.h"
#include "data/corpus.h"
#include "models/nematus.h"

#include "common/logging.h"
#include "command/config.h"

namespace marian {

  //template <class Builder>
  //class GraphGroup {
  //  private:
  //    ThreadPool pool_;
  //    
  //    Ptr<Builder> builder_;
  //    
  //  
  //  public:
  //    GraphGroup(size_t num) : pool_(num) {}
  //    
  //    float update(Ptr<data::CorpusBatch> batch) {
  //      auto task = [=]() {
  //        
  //        builder_->build(graph, batch);
  //        if(graph->getDevice() == masterDevice_) {
  //          
  //          this->
  //          opt->update(params, grads);
  //          this->distributeParams(graph);
  //        }
  //      }
  //      
  //      pool_.enqueue(task);
  //    }
  // }

  void TrainingLoop(Config& options,
                    Ptr<data::BatchGenerator<data::Corpus>> batchGenerator) {
 
 
    auto dimVocabs = options.get<std::vector<int>>("dim-vocabs");
    int dimEmb = options.get<int>("dim-emb");
    int dimRnn = options.get<int>("dim-rnn");
    int dimBatch = options.get<int>("mini-batch");
    int dimMaxiBatch = options.get<int>("maxi-batch");

    std::vector<int> dims = {
      dimVocabs[0], dimEmb, dimRnn,
      dimVocabs[1], dimEmb, dimRnn,
      dimBatch
    };

    auto devices = options.get<std::vector<int>>("device");
    
    std::mutex dispMutex;
    
    auto save = [](const std::string& name) {
      LOG(info) << "Saving parameters to " << name;
      //nematus->save(graph, name);
    };

    ThreadPool pool(devices.size(), devices.size());
 
    boost::timer::cpu_timer timer;

    size_t epochs = 1;
    size_t batches = 0;
    while((options.get<size_t>("after-epochs") == 0
           || epochs <= options.get<size_t>("after-epochs")) &&
          (options.get<size_t>("after-batches") == 0
           || batches < options.get<size_t>("after-batches"))) {

      batchGenerator->prepare(!options.get<bool>("no-shuffle"));

      float costSum = 0;
      size_t samples = 0;
      size_t wordsDisp = 0;

      while(*batchGenerator) {
        
        for(auto device : devices) {
        
          auto update = [&, device](Ptr<data::CorpusBatch> batch) {
            thread_local Ptr<ExpressionGraph> graph;
            thread_local Ptr<Nematus> nematus;
            thread_local Ptr<OptimizerBase> opt;
            
            if(!graph) {
              std::lock_guard<std::mutex> guard(dispMutex);
              graph = New<ExpressionGraph>();
              graph->setDevice(device);
              graph->reserveWorkspaceMB(options.get<size_t>("workspace"));
            }
            
            if(!nematus) {
              nematus = New<Nematus>(dims);
              if(options.has("init")) {
                LOG(info) << "Loading parameters from " << options.get<std::string>("init");
                nematus->load(graph, options.get<std::string>("init"));
              }
            }
          
            if(!opt) {
              Ptr<ClipperBase> clipper = nullptr;
              float clipNorm = options.get<double>("clip-norm");
              float lrate = options.get<double>("lrate");
              if(clipNorm > 0)
                clipper = Clipper<Norm>(clipNorm);
              opt = Optimizer<Adam>(lrate, keywords::clip=clipper);
            }
            
            auto costNode = nematus->construct(graph, batch);
            opt->update(graph);
            float cost =  costNode->scalar();
            
            std::lock_guard<std::mutex> guard(dispMutex);
        
            costSum += cost;
            samples += batch->size();
            wordsDisp += batch->words();
            batches++;
            //if(options.get<size_t>("after-batches")
            //   && batches >= options.get<size_t>("after-batches"))
            //  break;
    
            if(batches % options.get<size_t>("disp-freq") == 0) {
              std::stringstream ss;
              ss << "Ep. " << epochs
                 << " : Up. " << batches
                 << " : Sen. " << samples
                 << " : Cost " << std::fixed << std::setprecision(2)
                               << costSum / options.get<size_t>("disp-freq")
                 << " : Time " << timer.format(2, "%ws");
    
              float seconds = std::stof(timer.format(5, "%w"));
              float wps = wordsDisp /   (float)seconds;
    
              ss << " : " << std::fixed << std::setprecision(2)
                 << wps << " words/s";
    
              LOG(info) << ss.str();
    
              timer.start();
              costSum = 0;
              wordsDisp = 0;
            }
    
            if(batches % options.get<size_t>("save-freq") == 0) {
              if(options.get<bool>("overwrite"))
                save(options.get<std::string>("model") + ".npz");
              else
                save(options.get<std::string>("model") + "." + std::to_string(batches) + ".npz");
            }
            
          };
          
          auto batch = batchGenerator->next();
          pool.enqueue(update, batch);
        }
      }
      epochs++;
      LOG(info) << "Starting epoch " << epochs << " after " << samples << " samples";
    }
    LOG(info) << "Training finshed";
    save(options.get<std::string>("model") + ".npz");
    LOG(info) << timer.format(2, "%ws");
  }
}

int main(int argc, char** argv) {
  using namespace marian;
  using namespace data;
  using namespace keywords;

  std::shared_ptr<spdlog::logger> info;
  info = spdlog::stderr_logger_mt("info");
  info->set_pattern("[%Y-%m-%d %T] %v");

  Config options(argc, argv);
  std::cerr << options << std::endl;

  auto dimVocabs = options.get<std::vector<int>>("dim-vocabs");
  int dimEmb = options.get<int>("dim-emb");
  int dimRnn = options.get<int>("dim-rnn");
  int dimBatch = options.get<int>("mini-batch");
  int dimMaxiBatch = options.get<int>("maxi-batch");
  
  auto trainSets = options.get<std::vector<std::string>>("trainsets");
  auto vocabs = options.get<std::vector<std::string>>("vocabs");
  size_t maxSentenceLength = options.get<size_t>("max-length");
  auto corpus = New<Corpus>(trainSets, vocabs, dimVocabs, maxSentenceLength);
  auto bg = New<BatchGenerator<Corpus>>(corpus, dimBatch, dimMaxiBatch);
 
  TrainingLoop(options, bg);

  return 0;
}
