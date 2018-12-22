#include "optimizers.h"

#include "common/io.h"
#include "tensors/tensor_operators.h"
#include <array>

namespace marian {

void OptimizerBase::update(Tensor params, Tensor grads, size_t mbSize) {
  size_t refMBWords = refMBWordsParam_;
  if (refMBWords == 0) { // optimizer not configured to use hyper-parameter auto-adjustment
    refMBWords = mbSize = 1; // neutral settings that keep the standard behavior
  }
  else { // optimizer is configured to auto-adjust hyper-parameters
    ABORT_IF(mbSize == mbSizeNotProvided, "Using rational optimizer auto-adjustment with trainer that does not provide MB size");
    // note: this behavior is only meaningful if using the ce-sum criterion
  }

  // if true model for forward/backward uses a different type than the optimizer
  bool castOptimizerType = params->type() != optimizerType_;

  int elements = (int)params->size();

  // use an exponentially smoothed set of parameters
  if(mvAvg_) {
    // allocate storage for shards
    if(!optAlloc_) {
      optAlloc_ = New<TensorAllocator>(params->getBackend());
      // if optimizer type is different allocate also space for conversion tensors
      // and master parameter copy
      int numAllocateShards = castOptimizerType ? 3 : 1;
      optAlloc_->reserveExact(numAllocateShards * elements * sizeOf(optimizerType_));
      optAlloc_->allocate(avg_, {1, elements}, optimizerType_);
    }
  }

  if(castOptimizerType) {
    // if no smoothing is used, shards for conversion get allocated here
    if(!optAlloc_) {
      optAlloc_ = New<TensorAllocator>(params->getBackend());
      optAlloc_->reserveExact(2 * elements * sizeOf(optimizerType_));
    }

    if(!pm_) {
      // create parameter master copy and temporary gradient shard
      optAlloc_->allocate(pm_, {1, elements}, optimizerType_);
      optAlloc_->allocate(gd_, {1, elements}, optimizerType_);

      // keep parameter master copy around and initialize once, converting types
      CopyCast(pm_, params);
    }

    // overwrite temporary gradients with every update
    CopyCast(gd_, grads);
  } else {
    // no conversion, just assign at each update
    pm_ = params;
    gd_ = grads;
  }

  using namespace functional;

  // reverse cost scaling when used
  if(costScale_ != 1.f) {
    Element(_1 = _1 / costScale_, gd_);
  }

  // clip gradiens when used
  // @TODO, also check for nan?
  if(clipper_)
    clipper_->clip(grads);

  // perform update on master copy with cast gradients
  // if a type cast has been performed. Otherwise the
  // original tensors are used.
  updateImpl(pm_, gd_, mbSize, refMBWords);

  // @TODO: update moving average here
  if(mvAvg_)
    updateAvgParams(avg_, pm_, batchesSeen_, mbSize);

  // undo paramter type cast if required
  if(castOptimizerType) {
    CopyCast(params, pm_);
  }

  params->getBackend()->synchronize();
}

void Sgd::updateImpl(Tensor params, Tensor grads, size_t actualMBSize, size_t refMBWords) {
  actualMBSize, refMBWords; // (no correction for base update needed beyond using ce-sum)
  using namespace functional;
  Element(_1 -= eta_ * _2, params, grads);
}

// Adagrad update rule
void Adagrad::updateImpl(Tensor params, Tensor grads, size_t actualMBSize, size_t refMBWords) {
  ABORT_IF(actualMBSize != refMBWords, "Adagrad does not support rational hyper-parameter adjustment");

  // allocate optimizer-specific parameters
  if(!alloc_)
    alloc_ = New<TensorAllocator>(params->getBackend());

  if(!gt_) {
    int elements = (int)params->size();
    alloc_->reserveExact(params->memory()->size());
    alloc_->allocate(gt_, {1, elements}, params->type());
    gt_->set(0.f);
  }

  using namespace functional;
  Element(_1 += (_2 * _2), gt_, grads);

  // make sure eps_ does not drop below minimum value
  eps_ = std::max(NumericLimits<float>(params->type()).min * 2.f, eps_);
  Element(_1 -= (eta_ / (sqrt(_2) + eps_)) * _3, params, gt_, grads);
}

void Adagrad::load(const std::string& name,
                   const std::vector<Ptr<OptimizerBase>>& opts,
                   const std::vector<Ptr<Backend>>& backends,
                   const ScatterStateFunc& scatterFn) {
  ABORT_IF(opts.size() != backends.size(), "opts and backends of different sizes??");

  if(!filesystem::exists(name))
    return;

  LOG(info, "Loading Adagrad parameters from {}", name);

  std::vector<float> vGt;

  auto items = io::loadItems(name);
  for(auto item : items) {
    // get the size of gt_
    auto totalSize = item.shape.elements();

    // extract data into vectors
    if(item.name == "adagrad_gt") {
      vGt.resize(totalSize);
      std::copy(
          (float*)item.data(), ((float*)item.data()) + totalSize, vGt.begin());
    }
  }
  if(vGt.empty()) {
    LOG(warn, "[warn] Adagrad parameters not found in .npz file");
    return;
  }

  scatterFn(vGt,
    [&](size_t localDeviceIndex, std::vector<float>::const_iterator begin, std::vector<float>::const_iterator end) {
    auto opt = std::dynamic_pointer_cast<Adagrad>(opts[localDeviceIndex]);
    if(!opt->gt_) {
      if(!opt->alloc_)
        opt->alloc_ = New<TensorAllocator>(backends[localDeviceIndex]);
      auto size = end-begin;
      opt->alloc_->reserveExact(sizeof(float) * size);
      opt->alloc_->allocate(opt->gt_, {1, (int)size});
    }
    opt->gt_->set(std::vector<float>(begin, end));
  });
}

void Adagrad::save(const std::string& name,
                   const std::vector<Ptr<OptimizerBase>>& opts,
                   const GatherStateFunc& gatherFn,
                   bool isMainProcess /*= true*/) {
  LOG(info, "Saving Adagrad parameters to {}", name);

  // fetch and concatenate state vectors from distributed shards into a CPU-side vector
  auto vGt = gatherFn([&](size_t localDeviceIndex) {
      auto opt = std::dynamic_pointer_cast<Adagrad>(opts[localDeviceIndex]);
      std::vector<float> data;
      opt->gt_->get(data);
      return data;
    });

  // if not main MPI process then we have done our duty
  if (!isMainProcess)
    return;

  // save to file
  io::Item item;
  item.name = "adagrad_gt";
  item.shape = Shape({1, (int)vGt.size()});
  item.type = Type::float32;
  item.bytes.resize(vGt.size() * sizeOf(item.type));
  std::copy(
      (char*)vGt.data(), (char*)(vGt.data() + vGt.size()), item.bytes.begin());

  io::saveItems(name, {item});
}

void Adagrad::resetStats() {
  if(gt_)
    gt_->set(0.f);
}

// Adam
void Adam::updateImpl(Tensor params, Tensor grads, size_t actualMBSize, size_t refMBWords) {
  // lazy allocation
  if(!alloc_)
    alloc_ = New<TensorAllocator>(params->getBackend());

  if(!mt_) {
    int elements = (int)params->size();
    alloc_->reserveExact(2 * elements * sizeOf(params->type()));
    alloc_->allocate(mt_, {1, elements}, params->type());
    mt_->set(0.f);

    alloc_->allocate(vt_, {1, elements}, params->type());
    vt_->set(0.f);
  }

  double T    = (double)actualMBSize;
  double Tref = (double)refMBWords;

  // adjust for minibatch-size changes if Adam parameters are given a reference size (else do nothing)
  double eta   = eta_ * (T/Tref);
  double beta1 = beta1_;
  double beta2 = beta2_;
  double decay = w_    ;

  // denominators. At steady state: =1. This recursion does the same as the Adam beta correction term.
  denom1_ = (beta1 * denom1_) + (1 - beta1); // momentum smoothing
  denom2_ = (beta2 * denom2_) + (1 - beta2); // RMS normalization

  //LOG_ONCE(info, "[adam] First update: Tref = {}, T = {}, eta = {} -> {}, beta = {}, {}", Tref, T, eta_, eta, beta1, beta2);

  // numerators. Divide by T to convert ce-sum gradient to avg gradient.
  using namespace functional;
  Element(_1 = ((float)beta1 * _1) + float((1 - beta1) / T    ) *  _2,       mt_, grads); // momentum smoothing. At steady state: =smoothed avg gradient
  Element(_1 = ((float)beta2 * _1) + float((1 - beta2) / T / T) * (_2 * _2), vt_, grads); // RMS normalization.  At steady state: =mean square of the avg gradients

  // make sure eps_ does not drop below minimum value, this is important
  // when training with mixed precision. Otherwise we divide by 0.
  // We multiply the minimum by 2 in order to step away from the abyss.
  //eps_ = std::max(NumericLimits<float>(params->type()).min * 2.f, eps_);

  // apply Adam normalization
  float etaf = (float)eta, denom1f = (float)denom1_, denom2f = (float)denom2_, decayf = (float)decay; // (get casts out of Element expression for readability)
  Element(_1 -= etaf                               // learning-rate: x_t = x_{t-1} - \eta * (...)
                * ((  (     _2 / denom1f)          // momentum-smoothed per-sample gradient: m_{t-1}
                    / (sqrt(_3 / denom2f) + eps_)) // normalize by RMS: \sqrt(v_{t-1})
                   + decayf * _1),                 // weight-decay: w * x_{t-1}
          params,  // =_1
          mt_,     // =_2
          vt_      // =_3
          );
}

void Adam::load(const std::string& name,
                const std::vector<Ptr<OptimizerBase>>& opts,
                const std::vector<Ptr<Backend>>& backends,
                const ScatterStateFunc& scatterFn) {
  ABORT_IF(opts.size() != backends.size(), "opts and backends of different sizes??");

  if(!filesystem::exists(name))
    return;

  LOG(info, "Loading Adam parameters from {}", name);

  std::vector<float> vMt;
  std::vector<float> vVt;
  std::array<double, 2> vDenoms;

  auto items = io::loadItems(name);
  for(auto item : items) {
    // get the size of mt_ and vt_, they are the same
    auto totalSize = item.shape.elements();

    // extract data into vectors
    if(item.name == "adam_mt") {
      vMt.resize(totalSize);
      std::copy(
          (float*)item.data(), ((float*)item.data()) + totalSize, vMt.begin());
    }
    else if(item.name == "adam_vt") {
      vVt.resize(totalSize);
      std::copy(
          (float*)item.data(), ((float*)item.data()) + totalSize, vVt.begin());
    }
    else if(item.name == "adam_denoms") {
      ABORT_IF(totalSize != 2, "adam_denoms should have 2 entries");
      std::copy(
          (double*)item.data(), ((double*)item.data()) + totalSize, vDenoms.begin());
      // Back compat note: Old files lacked "adam_denoms". For those, vDenoms will remain 0, which reproduces the old behavior.
    }
  }
  if(vMt.empty() || vVt.empty()) {
    LOG(warn, "[warn] Adam parameters not found in .npz file");
    return;
  }
  ABORT_IF(vMt.size() != vVt.size(), "mt and vt have different sizes??");

  //LOG(info, "loading Adam params");
  scatterFn(vMt,
    [&](size_t localDeviceIndex, std::vector<float>::const_iterator begin, std::vector<float>::const_iterator end) {
    auto opt = std::dynamic_pointer_cast<Adam>(opts[localDeviceIndex]);
    if(!opt->mt_ || !opt->vt_) { // lazily allocate
      if(!opt->alloc_)
        opt->alloc_ = New<TensorAllocator>(backends[localDeviceIndex]);
      auto size = end-begin;
      opt->alloc_->reserveExact(2 * sizeof(float) * size);
      opt->alloc_->allocate(opt->mt_, {1, (int)size});
      opt->alloc_->allocate(opt->vt_, {1, (int)size});
    }
    opt->mt_->set(std::vector<float>(begin, end)); // set the value
  });

  scatterFn(vVt,
    [&](size_t id, std::vector<float>::const_iterator begin, std::vector<float>::const_iterator end) {
    auto opt = std::dynamic_pointer_cast<Adam>(opts[id]);
    opt->vt_->set(std::vector<float>(begin, end));
  });

  denom1_ = vDenoms[0];
  denom2_ = vDenoms[1];
  //LOG(info, "done loading Adam params");
}

void Adam::save(const std::string& name,
                const std::vector<Ptr<OptimizerBase>>& opts,
                const GatherStateFunc& gatherFn,
                bool isMainProcess /*= true*/) {
  if (isMainProcess)
    LOG(info, "Saving Adam parameters to {}", name);

  // fetch and concatenate state vectors from distributed shards into a CPU-side vector
  auto vMt = gatherFn([&](size_t localDeviceIndex) {
    auto opt = std::dynamic_pointer_cast<Adam>(opts[localDeviceIndex]);
    std::vector<float> data;
    opt->mt_->get(data);
    return data;
  });

  auto vVt = gatherFn([&](size_t localDeviceIndex) {
    auto opt = std::dynamic_pointer_cast<Adam>(opts[localDeviceIndex]);
    std::vector<float> data;
    opt->vt_->get(data);
    return data;
  });

  // if not main MPI process then we have done our duty
  if (!isMainProcess)
      return;

  // save to file
  io::Item itemMt;
  itemMt.name = "adam_mt";
  itemMt.shape = Shape({1, (int)vMt.size()});
  itemMt.type = Type::float32;
  itemMt.bytes.resize(vMt.size() * sizeOf(itemMt.type));
  std::copy(
      (char*)vMt.data(), (char*)(vMt.data() + vMt.size()), itemMt.bytes.begin());

  io::Item itemVt;
  itemVt.name = "adam_vt";
  itemVt.shape = Shape({1, (int)vVt.size()});
  itemVt.type = Type::float32;
  itemVt.bytes.resize(vVt.size() * sizeOf(itemVt.type));
  std::copy(
      (char*)vVt.data(), (char*)(vVt.data() + vVt.size()), itemVt.bytes.begin());

  // @TODO: this pattern is duplicated several times; refactor it
  std::array<double, 2> vDenoms{denom1_, denom2_};
  io::Item itemDenoms;
  itemDenoms.name = "adam_denoms";
  itemDenoms.shape = Shape({1, (int)vDenoms.size()});
  itemDenoms.type = Type::float64;
  itemDenoms.bytes.resize(vDenoms.size() * sizeOf(itemDenoms.type));
  std::copy(
      (char*)vDenoms.data(), (char*)(vDenoms.data() + vDenoms.size()), itemDenoms.bytes.begin());

  io::saveItems(name, {itemMt, itemVt, itemDenoms});
}

void Adam::resetStats() {
  if(mt_)
    mt_->set(0.f);

  if(vt_)
    vt_->set(0.f);

  denom1_ = 0; // @BUGBUG: or 1 or refMBWords if so specified. Fix once we have proper parameterization for that.
  denom2_ = 0;
}

Ptr<OptimizerBase> Optimizer(Ptr<Options> options) {
  auto optType = options->get<std::string>("optimizer");
  auto params = options->has("optimizer-params")
                     ? options->get<std::vector<float>>("optimizer-params")
                     : std::vector<float>({});
  Ptr<OptimizerBase> opt;
  if(optType == "sgd") {
    opt = New<Sgd>(options);
  } else if(optType == "adagrad") {
    opt = New<Adagrad>(options);
  } else if(optType == "adam") {
    opt = New<Adam>(options);
  } else {
    ABORT("Unknown optimizer kind: {}", opt);
  }

  opt->setParams(params);
  return opt;
}
}  // namespace marian
