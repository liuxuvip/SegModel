
#ifndef CAFFE_ChannelReduction_LAYER_HPP_
#define CAFFE_ChannelReduction_LAYER_HPP_

#include <vector>

#include "caffe/blob.hpp"
#include "caffe/layer.hpp"
#include "caffe/proto/caffe.pb.h"


namespace caffe {

template <typename Dtype>
class ChannelReductionLayer : public Layer<Dtype> {
 public:
  explicit ChannelReductionLayer(const LayerParameter& param): Layer<Dtype>(param) {}
  virtual inline const char* type() const { return "ChannelReduction"; }
	virtual void Forward_gpu(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top);
  virtual void Backward_gpu(const vector<Blob<Dtype>*>& top, const vector<Blob<Dtype>*>& bottom);
  virtual void SecForward_gpu(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top);
  
	virtual void LayerSetUp(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top);
  virtual void Reshape(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top);
 protected:
 	
};

}  // namespace caffe

#endif  // CAFFE_ChannelReductionLAYER_HPP_
