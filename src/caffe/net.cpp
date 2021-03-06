// Copyright Yangqing Jia 2013

#include <set>
#include <string>
#include <vector>

#include "caffe/proto/caffe.pb.h"
#include "caffe/layer_factory.hpp"
#include "caffe/net.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/format.hpp"
#include "caffe/layers/func/parallel_layer.hpp"
using std::pair;
using std::map;
using std::set;

namespace caffe {
#if 0
template <typename Dtype>
int Net<Dtype>::adam_iter_ = 0;
template <typename Dtype>
Dtype Net<Dtype>::momentum_power_ = -1;
template <typename Dtype>
Dtype Net<Dtype>::momentum2_power_ = -1;
#endif 

template <typename Dtype>
Net<Dtype>::Net(const NetParameter& param, const vector<shared_ptr<Blob<Dtype> > > net_input_blobs, const vector<string> net_input_blob_names):param_(param)
{
	adam_iter_ = 0;
	momentum_power_ = -1;
	momentum2_power_ = -1;
	
  if (Caffe::workspace_.size() == 0)
  {
		Caffe::workspace_.resize(12);
		for (int i=0;i<12;i++)
			Caffe::workspace_[i] = new Blob<Dtype>();
		
		Caffe::parallel_workspace_.resize(48);
		for (int i=0;i<48;i++)
			Caffe::parallel_workspace_[i] = new Blob<Dtype>();
  }

  
  name_ = param_.name();
  map<string, int> blob_name_to_idx;
  set<string> available_blobs;
	set<int> available_flows;

  blobs_.clear();
  blob_names_.clear();
  blob_loss_weights_.clear();
	
	input_blobs_.clear();
  input_blob_indices_.clear();
  available_blobs.clear();
  available_flows.clear();
  blob_name_to_idx.clear();
  
  
  //---------------------------------------------- get the net input blob ------------------------------------------
  input_blobs_.clear();
  input_blob_indices_.clear();
  for (int i = 0; i < net_input_blobs.size(); ++i)
  {
  	input_blob_indices_.push_back(i);
	  blobs_.push_back(net_input_blobs[i]);
	  blob_names_.push_back(net_input_blob_names[i]);
	  blob_loss_weights_.push_back(Dtype(0));
	  blob_name_to_idx[net_input_blob_names[i]] = blob_names_.size() - 1;
	  
	  available_blobs.insert(net_input_blob_names[i]);
	  input_blobs_.push_back(net_input_blobs[i].get());
  }
  
  
  
  //------------------------------------------------- build top and bottom ---------------------------------------------
  bottom_vecs_.resize(param_.layer_size());
  top_vecs_.resize(param_.layer_size());
  bottom_id_vecs_.resize(param_.layer_size());
  top_id_vecs_.resize(param_.layer_size());

  for (int i = 0; i < param_.layer_size(); ++i)
  {
    const LayerParameter& layer_param = param_.layer(i);
    if (layer_param.type() != "ParallelBatchNormalization")
    	layers_.push_back(shared_ptr<Layer<Dtype> >(new ParallelLayer<Dtype>(layer_param)));
    else
    	layers_.push_back(LayerRegistry<Dtype>::CreateLayer(layer_param));
    vector<string>::iterator iter = find(layer_names_.begin(),layer_names_.end(),layer_param.name());
    if (iter != layer_names_.end())
    		LOG(FATAL)<<"Duplicate layer name "<<layer_param.name();
    layer_names_.push_back(layer_param.name());
    layer_need_backward_.push_back(layer_param.include().need_backward());
    LOG(INFO) << "Creating Layer " << layer_param.name();
		//--------------------------- bottom ---------------------------------
    for (int j = 0; j < layer_param.bottom_size(); ++j)
    {
      const string& blob_name = layer_param.bottom(j);
      const int flow_ind = layer_param.bottom_flow_size()>j? layer_param.bottom_flow(j):-1;
      if (available_flows.find(flow_ind) == available_flows.end() && flow_ind != -1)
        LOG(FATAL) << "Unknown input flow  "<< flow_ind / NGPUS<< " to layer "<<layer_param.name();
      if (available_blobs.find(blob_name) == available_blobs.end())
        LOG(FATAL) << "Unknown input blob  "<< blob_name << " to layer "<<layer_param.name();

      LOG(INFO) << layer_param.name() << " <- " << blob_name;
      bottom_vecs_[i].push_back(blobs_[blob_name_to_idx[blob_name]].get());
      bottom_id_vecs_[i].push_back(blob_name_to_idx[blob_name]);
      available_blobs.erase(blob_name);
      if (flow_ind != -1)
      	available_flows.erase(flow_ind);
    }
    //--------------------------- top ---------------------------------
    for (int j = 0; j < layer_param.top_size(); ++j)
    {
      const string& blob_name = layer_param.top(j);
      const int flow_ind = layer_param.top_flow_size()>j? layer_param.top_flow(j):-1;
      bool in_place = false;
      if (blob_name_to_idx.find(blob_name) != blob_name_to_idx.end())
      {
        for (int k=0;k<layer_param.bottom_size();k++)
        {
          if (layer_param.bottom(k) == blob_name)
          {
            in_place = true;
            break;
          }
        }
        if (!in_place)
          LOG(FATAL) << "Duplicate blobs "<<blob_name<<" produced by multiple sources.";
      }


      LOG(INFO) << layer_param.name() << " -> " << blob_name;
      if (in_place)
      {
        //-------------previous blob-----------------------
        available_blobs.insert(blob_name);
        available_flows.insert(flow_ind);
        top_vecs_[i].push_back(blobs_[blob_name_to_idx[blob_name]].get());
        top_id_vecs_[i].push_back(blob_name_to_idx[blob_name]);
      }
      else
      {
        //-------------new data blob----------------------
        shared_ptr<Blob<Dtype> > blob_pointer(new Blob<Dtype>());
        blobs_.push_back(blob_pointer);
        blob_names_.push_back(blob_name);
        blob_loss_weights_.push_back(layer_param.include().loss_weight());
        blob_name_to_idx[blob_name] = blob_names_.size() - 1;

        available_blobs.insert(blob_name);
        available_flows.insert(flow_ind);
        top_vecs_[i].push_back(blobs_[blob_name_to_idx[blob_name]].get());
        top_id_vecs_[i].push_back(blob_name_to_idx[blob_name]);
      }
    }
  }

  //------------------------------------------------- set up layers ---------------------------------------------
  for (set<string>::iterator it = available_blobs.begin(); it != available_blobs.end(); ++it)
  {
    LOG(ERROR) << "This network produces output " << *it;
    output_blob_indices_.push_back(blob_name_to_idx[*it]);
    output_blobs_.push_back(blobs_[blob_name_to_idx[*it]].get());
  }

  LOG(ERROR) << "Setting up the layers.";
  for (int i = 0; i < layers_.size(); ++i)
  {
    LOG(INFO) << "Setting up " << layer_names_[i];
    layers_[i]->SetUp(bottom_vecs_[i], top_vecs_[i]);
    vector<shared_ptr<Blob<Dtype> > >& layer_blobs = layers_[i]->blobs();
    vector<shared_ptr<Blob<Dtype> > >& parallel_layer_blobs = layers_[i]->parallel_blobs();
    if (parallel_layer_blobs.size() > 0)
    	CHECK_EQ(layer_blobs.size()*NGPUS,parallel_layer_blobs.size());
    CHECK_EQ(layer_blobs.size(),layers_[i]->lr_mult().size());
    CHECK_EQ(layer_blobs.size(),layers_[i]->decay_mult().size());
    if (layers_[i]->lr_mult().size()>0)
    {	
    	bool back_ward = false;
    	for (int j=0;j<layers_[i]->lr_mult().size();j++)
    		back_ward |= (layers_[i]->lr_mult()[j]>0);
    	if (!back_ward)
    		LOG(INFO)<<"layer "<<layer_names_[i]<<" frozens paramters";
    }
    for (int topid = 0; topid < top_vecs_[i].size(); ++topid)
      LOG(INFO) << "Top shape: " << top_vecs_[i][topid]->num()
                << " "  << top_vecs_[i][topid]->channels()
                << " " << top_vecs_[i][topid]->height()
                << " "  << top_vecs_[i][topid]->width();

  }
//--------------------------	
LOG(INFO)<<"*************************** computing memory ***************************************";
	
	Dtype memory_cost_data = 0;
	Dtype max_diff = 0;
	for (int i=0;i<blobs_.size();i++)
	{
		memory_cost_data += blobs_[i]->count();
		if (max_diff < blobs_[i]->count())
			 max_diff = blobs_[i]->count();
	}	
	LOG(INFO)<<"Data cost memory is "<<memory_cost_data/(1024*1024*1024)*sizeof(Dtype)<<"GB";
	LOG(INFO)<<"Diff cost memory is "<<max_diff*param_.num_flow()/(1024*1024*1024)*sizeof(Dtype)<<"GB";

	
	LOG(INFO)<<"Totol memeory cost is "<<(memory_cost_data+max_diff*param_.num_flow())*sizeof(Dtype)/(1024*1024*1024)<<"GB";
LOG(INFO)<<"*************************************************************************************";
//----------------------------------------------
  LOG(ERROR) << "Network initialization done.";
  if (param_.num_flow() > 0)
  {
		tensor_flows_.resize(param_.num_flow()*NGPUS);
		tensor_flows_temp_.resize(param_.num_flow()*NGPUS);
		for (int i=0;i<tensor_flows_.size();i++)
		{
		  tensor_flows_[i].reset(new Blob<Dtype>(1,1,1,1));
		  tensor_flows_temp_[i].reset(new Blob<Dtype>(1,1,1,1));
		}
		flow_flag.resize(param_.num_flow()*NGPUS,true);
	}	
	
	for (int i=0;i<layers_.size();i++)
	{
		layers_[i]->first_moment().resize(layers_[i]->blobs().size());
		layers_[i]->second_moment().resize(layers_[i]->blobs().size());

		for (int j=0;j<layers_[i]->blobs().size();j++)
		{
			int num = layers_[i]->blobs()[j]->num();
			int channels = layers_[i]->blobs()[j]->channels();
			int height = layers_[i]->blobs()[j]->height();
			int width = layers_[i]->blobs()[j]->width();
			layers_[i]->first_moment()[j].reset(new Blob<Dtype>(num,channels,height,width));
			layers_[i]->second_moment()[j].reset(new Blob<Dtype>(num,channels,height,width));
		}
	}
}

template <typename Dtype>
Dtype Net<Dtype>::Forward()
{
	Dtype loss = 0;
  for (int i = 0; i < layers_.size(); ++i)
  {
   	//LOG(INFO)<<"fowarding layer "<<layer_names_[i];
    layers_[i]->Reshape(bottom_vecs_[i], top_vecs_[i]);   
    /*------------------------------------------------------------------------------*/
    if ((Caffe::reuse() == true||!layer_need_backward_[i]) && param_.num_flow() > 0)
    {  	 
  		CHECK_EQ(param_.layer(i).bottom_flow_size(),bottom_vecs_[i].size());
  		CHECK_EQ(param_.layer(i).top_flow_size(),top_vecs_[i].size());
    	
      /*----- set the point to the flow------------ */
      for (int j=0;j<bottom_vecs_[i].size();j++)
      {
        int current_flow = param_.layer(i).bottom_flow(j);              
        if (current_flow < 0)
      		continue;
        
        CHECK_LE(current_flow,tensor_flows_.size()-1);
        
        if (flow_flag[current_flow])
          bottom_vecs_[i][j]->set_data(*tensor_flows_[current_flow]);
        else
          bottom_vecs_[i][j]->set_data(*tensor_flows_temp_[current_flow]);

      }
      //new head for the flow
      for (int j=0;j<top_vecs_[i].size();j++)
      {
        int current_flow;
        if (param_.layer(i).top_flow_size() > j)
        	current_flow = param_.layer(i).top_flow(j);
        else
          break;
        
        if (current_flow < 0)
      		continue;
        
        bool inplace = false;
        for (int k=0;k<param_.layer(i).bottom_size();k++)// check if in-place computation
        {
          if (param_.layer(i).top(j) == param_.layer(i).bottom(k))
          {
            if (flow_flag[current_flow])
              top_vecs_[i][j]->set_data(*tensor_flows_[current_flow]);
            else
              top_vecs_[i][j]->set_data(*tensor_flows_temp_[current_flow]);

            inplace = true;
            break;
          }
        }

        if (!inplace) // move the top to the other data blob
        {
          if (flow_flag[current_flow])
          {
            top_vecs_[i][j]->set_data(*tensor_flows_temp_[current_flow]);
            flow_flag[current_flow] = false;
          }
          else
          {
            top_vecs_[i][j]->set_data(*tensor_flows_[current_flow]);
            flow_flag[current_flow] = true;
          }
        }
      }
    }  
    /*------------------------------------------------------------------------------*/   		
    loss += layers_[i]->Forward(bottom_vecs_[i], top_vecs_[i]);  
  }
  //if (Caffe::gan_type() == "train_gnet")
  // 	LOG(FATAL)<<"--------------";
  return loss;
}

template <typename Dtype>
void Net<Dtype>::Backward()
{
  for (int i = layers_.size() - 1; i >= 0; --i)
  {  	
  	//LOG(INFO)<<"backwarding layer "<<layer_names_[i];
  	layers_[i]->Reshape(bottom_vecs_[i], top_vecs_[i]);
    if (!layer_need_backward_[i])
      continue;
    if (Caffe::reuse() == true && param_.num_flow() > 0)
    {
    	
  		CHECK_EQ(param_.layer(i).bottom_flow_size(),bottom_vecs_[i].size());
  		CHECK_EQ(param_.layer(i).top_flow_size(),top_vecs_[i].size());
    	
    	
/*--------------------- set the point of the gradient to the flow---------------------- */
		  for (int j=0;j<top_vecs_[i].size();j++)
		  {
		  	int current_flow = param_.layer(i).top_flow(j);	    
		    if (current_flow < 0)
		    	continue;

		    CHECK_LE(current_flow,tensor_flows_.size()-1);
		    
		    if (flow_flag[current_flow])
		      top_vecs_[i][j]->set_diff(*tensor_flows_[current_flow]);
		    else
		      top_vecs_[i][j]->set_diff(*tensor_flows_temp_[current_flow]);
		  }
		  //new head for the gradient flow
		  for (int j=0;j<bottom_vecs_[i].size();j++)
		  {
		    int current_flow;
		    if (param_.layer(i).bottom_flow_size() > j)
		    	current_flow = param_.layer(i).bottom_flow(j);
		    else	
		      break;
		    
		    if (current_flow < 0)
		    	continue;

		    bool inplace = false;
		    for (int k=0;k<param_.layer(i).top_size();k++)// check if in-place computation
		    {
		      if (param_.layer(i).bottom(j) == param_.layer(i).top(k))
		      {
		        if (flow_flag[current_flow])
		          bottom_vecs_[i][j]->set_diff(*tensor_flows_[current_flow]);
		        else
		          bottom_vecs_[i][j]->set_diff(*tensor_flows_temp_[current_flow]);

		        inplace = true;
		        break;
		      }
		    }

		    if (!inplace) // move the bottom to the other gradient blob
		    {
		      if (flow_flag[current_flow])
		      {
		        bottom_vecs_[i][j]->set_diff(*tensor_flows_temp_[current_flow]);
		        flow_flag[current_flow] = false;
		      }
		      else
		      {
		        bottom_vecs_[i][j]->set_diff(*tensor_flows_[current_flow]);
		        flow_flag[current_flow] = true;
		      }
		    }
		  }
/* ------------------------------------------------------------------------------------ */          
		} 
    layers_[i]->Backward(top_vecs_[i], bottom_vecs_[i]);
    //if (top_vecs_[i].size() > 0)  
    //{
    //	Dtype sum = 0;
    //	caffe_gpu_asum(top_vecs_[i][0]->count(),top_vecs_[i][0]->gpu_diff(),&sum);
    //	LOG(INFO)<<sum;
  	//}
  }  
}

template <typename Dtype>
void Net<Dtype>::SecForward()
{
  for (int i = 0; i < layers_.size(); ++i)
  {
   	//LOG(INFO)<<"secfowarding layer "<<layer_names_[i];
    layers_[i]->Reshape(bottom_vecs_[i], top_vecs_[i]);     
    /*------------------------------------------------------------------------------*/
    if (param_.num_flow() > 0)
    {  	 
  		CHECK_EQ(param_.layer(i).bottom_flow_size(),bottom_vecs_[i].size());
  		CHECK_EQ(param_.layer(i).top_flow_size(),top_vecs_[i].size());
    	
      /*----- set the point to the flow------------ */
      for (int j=0;j<bottom_vecs_[i].size();j++)
      {
        int current_flow = param_.layer(i).bottom_flow(j);              
        if (current_flow < 0)
      		continue;
        
        CHECK_LE(current_flow,tensor_flows_.size()-1);
        
        if (flow_flag[current_flow])
          bottom_vecs_[i][j]->set_sec_diff(*tensor_flows_[current_flow]);
        else
          bottom_vecs_[i][j]->set_sec_diff(*tensor_flows_temp_[current_flow]);

      }


      //new head for the flow
      for (int j=0;j<top_vecs_[i].size();j++)
      {
        int current_flow;
        if (param_.layer(i).top_flow_size() > j)
        	current_flow = param_.layer(i).top_flow(j);
        else
          break;
        
        if (current_flow < 0)
      		continue;
        
        bool inplace = false;
        for (int k=0;k<param_.layer(i).bottom_size();k++)// check if in-place computation
        {
          if (param_.layer(i).top(j) == param_.layer(i).bottom(k))
          {
            if (flow_flag[current_flow])
              top_vecs_[i][j]->set_sec_diff(*tensor_flows_[current_flow]);
            else
              top_vecs_[i][j]->set_sec_diff(*tensor_flows_temp_[current_flow]);

            inplace = true;
            break;
          }
        }

        if (!inplace) // move the top to the other secdiff blob
        {
          if (flow_flag[current_flow])
          {
            top_vecs_[i][j]->set_sec_diff(*tensor_flows_temp_[current_flow]);
            flow_flag[current_flow] = false;
          }
          else
          {
            top_vecs_[i][j]->set_sec_diff(*tensor_flows_[current_flow]);
            flow_flag[current_flow] = true;
          }
        }
      }
    }  
    /*------------------------------------------------------------------------------*/   		
    layers_[i]->SecForward(bottom_vecs_[i], top_vecs_[i]);
    //if (top_vecs_[i].size() > 0)  
   // {
    //	Dtype sum = 0;
    //	caffe_gpu_asum(top_vecs_[i][0]->count(),top_vecs_[i][0]->gpu_sec_diff(),&sum);
    //	LOG(INFO)<<sum;
  	//}
  }
}

//----------------------------------------------------------------------------
template <typename Dtype>
Dtype Net<Dtype>::GetLearningRate(int iter, int max_iter) 
{
  Dtype rate;
  const string& lr_policy = optimizer_.lr_policy();
  if (lr_policy == "fixed") 
    rate = optimizer_.base_lr();
  else if (lr_policy == "step")
  { 
    Dtype current_step = iter / optimizer_.stepsize();
    rate = optimizer_.base_lr() * pow(optimizer_.gamma(), current_step);
  }
  else if (lr_policy == "multistep") 
  {
  	Dtype current_step = 0;
  	
  	while (iter > optimizer_.stepvalue(current_step) && current_step < optimizer_.stepvalue_size())
  		current_step ++;
  		
  	rate = optimizer_.base_lr() * pow(optimizer_.gamma(),current_step);
  		
  }
  else if (lr_policy == "poly") 
    rate = optimizer_.base_lr() * pow(Dtype(1.) - (Dtype(iter) / Dtype(max_iter)), optimizer_.power());
 	else if (lr_policy == "lineardecay") 
    rate = optimizer_.base_lr() * (Dtype(1.) - (Dtype(iter) / Dtype(max_iter)));
    
  return rate;
}

template <typename Dtype>
void Net<Dtype>::Update(int iter, int max_iter, bool display)
{
	Dtype rate = GetLearningRate(iter, max_iter);
	if (display)
    LOG(INFO) << "Iteration " << iter << ", lr = " << rate;
  
	if (optimizer_.type() == "SGD")
	{
		for (int i = 0; i < layers_.size(); i++)
			for (int j = 0;j < layers_[i]->blobs().size();j++)
			{
				Blob<Dtype>*  param_blob = layers_[i]->blobs()[j].get();
				Dtype local_weight_decay = layers_[i]->decay_mult()[j]; 
				Dtype local_lr = layers_[i]->lr_mult()[j]; 
				Blob<Dtype>* first_moment = layers_[i]->first_moment()[j].get(); 
				
				caffe_gpu_axpy(param_blob->count(),
							  Dtype(optimizer_.weight_decay() * local_weight_decay), param_blob->gpu_data(),
							  param_blob->mutable_gpu_diff());            
					 		
				caffe_gpu_axpby(param_blob->count(), 
								Dtype(rate * local_lr), param_blob->gpu_diff(), 
								Dtype(optimizer_.momentum()),  first_moment->mutable_gpu_data());
						
				caffe_copy(param_blob->count(),  first_moment->gpu_data(), param_blob->mutable_gpu_diff());
		
				caffe_gpu_add(param_blob->count(), Dtype(1), param_blob->gpu_data(), Dtype(-1), param_blob->gpu_diff(),
					            param_blob->mutable_gpu_data());	            
			}
	}
	else if (optimizer_.type() == "Adam")
	{			
		if (momentum_power_ == -1)
			momentum_power_ = optimizer_.momentum();
		if (momentum2_power_ == -1)
			momentum2_power_ = optimizer_.momentum2();
				
		for (int i = 0; i < layers_.size(); i++)
			for (int j = 0;j < layers_[i]->blobs().size();j++)
			{
				
				Blob<Dtype>*  param_blob = layers_[i]->blobs()[j].get();
				Dtype local_weight_decay = layers_[i]->decay_mult()[j]; 
				Dtype local_lr = layers_[i]->lr_mult()[j]; 
				Blob<Dtype>* first_moment = layers_[i]->first_moment()[j].get(); 
				Blob<Dtype>* second_moment = layers_[i]->second_moment()[j].get(); 
				
				const Dtype correction = std::sqrt(Dtype(1) - momentum2_power_) / (Dtype(1.) - momentum_power_);
				adam_update_gpu(param_blob->count(), param_blob->mutable_gpu_diff(),
					first_moment->mutable_gpu_data(), second_moment->mutable_gpu_data(), Dtype(optimizer_.momentum()), 
					Dtype(optimizer_.momentum2()), Dtype(optimizer_.delta()), correction);     
				caffe_gpu_add(param_blob->count(), Dtype(1), param_blob->gpu_data(), -Dtype(rate *local_lr), param_blob->gpu_diff(),
							        param_blob->mutable_gpu_data());		  
			}   
		momentum_power_ *= optimizer_.momentum();   
		momentum2_power_ *= optimizer_.momentum2();  
		adam_iter_++;
	}	
	else
		LOG(FATAL)<<"unspported optimization type";                                  
}
template <typename Dtype>
void Net<Dtype>::BcastData()
{
	if (NGPUS > 1)
	{			
		for (int i = 0; i < layers_.size(); i++)
			for (int j = 0;j < layers_[i]->blobs().size();j++)
			{
				CUDA_SYNCHRONIZE;	
				for (int k = 0; k < NGPUS; k++) 
				{
					CUDA_CHECK(cudaSetDevice(Caffe::GPUs[k]));
					ncclBcast((void *)layers_[i]->parallel_blobs()[j*NGPUS+k]->mutable_gpu_data(),layers_[i]->parallel_blobs()[j*NGPUS+k]->count(),
															ncclFloat,0,Caffe::comms(k),Caffe::stream(k));		
				}
				CUDA_SYNCHRONIZE;
			}
					
	}
}
template <typename Dtype>
void Net<Dtype>::ReduceDiff()
{
	if (NGPUS > 1)
	{			
		for (int i = 0; i < layers_.size(); i++)
			for (int j = 0;j < layers_[i]->blobs().size();j++)
			{
				CUDA_SYNCHRONIZE;	
				for (int k = 0; k < NGPUS; k++) 
				{
					CUDA_CHECK(cudaSetDevice(Caffe::GPUs[k]));
					ncclReduce(layers_[i]->parallel_blobs()[j*NGPUS+k]->gpu_diff(),layers_[i]->parallel_blobs()[j*NGPUS]->mutable_gpu_diff(),
															layers_[i]->parallel_blobs()[j*NGPUS]->count(),
															ncclFloat,ncclSum,0,Caffe::comms(k),Caffe::stream(k));			
				}
				CUDA_SYNCHRONIZE;		
			}
	}
}
template <typename Dtype>
void Net<Dtype>::ClearParamDiffs()
{
	for (int i = 0; i < layers_.size(); i++)
		for (int j = 0;j < layers_[i]->blobs().size();j++)
		{
			caffe_gpu_set(layers_[i]->blobs()[j]->count(), Dtype(0), layers_[i]->blobs()[j]->mutable_gpu_diff());
			if (NGPUS > 1)
			{
				for (int k = 0; k < NGPUS; k++) 
				{
					CUDA_CHECK(cudaSetDevice(Caffe::GPUs[k]));
					caffe_gpu_set(layers_[i]->parallel_blobs()[j*NGPUS+k]->count(), Dtype(0), layers_[i]->parallel_blobs()[j*NGPUS+k]->mutable_gpu_diff());
				}
			}
			CUDA_CHECK(cudaSetDevice(Caffe::GPUs[0])); 
		}

}


//------------------------------------------------------------------------------------------
template <typename Dtype>
void Net<Dtype>::ToProto(NetParameter* param, bool write_diff)
{
  param->Clear();
  param->set_name(name_);
  LOG(INFO) << "Serializing " << layers_.size() << " layers";
  for (int i = 0; i < layers_.size(); ++i)
  {
    LayerParameter* layer_param = param->add_layer();
    layers_[i]->ToProto(layer_param, write_diff);
  }
}
template <typename Dtype>
void Net<Dtype>::CopyTrainedLayersFrom(const NetParameter& param)
{
  vector<bool> layer_used_flag(param.layer_size(), false);
  LOG(INFO)<<"............................... there are "<<param.layer_size()<<"..........in this model...................";
  for (int i = 0; i < layer_names_.size();i++)
  {
    int source_layer_id = 0;
    while (source_layer_id != param.layer_size() && layer_names_[i] != param.layer(source_layer_id).name() && layer_names_[i] != "fixed_"+param.layer(source_layer_id).name())
      ++source_layer_id;
		
		const LayerParameter& source_layer = param.layer(source_layer_id);
    const string& source_layer_name = source_layer.name();
		
    if (source_layer_id == param.layer_size())
    {	
    	if (layers_[i]->blobs().size())
      	LOG(INFO) << "Target layer " << layer_names_[i] << " not initialized.";
      continue;
    }
    layer_used_flag[source_layer_id] = true;
    if (layers_[i]->blobs().size())
    	LOG(INFO) << "Loading source layer " << source_layer_name;
    else
    	continue;
    vector<shared_ptr<Blob<Dtype> > >& target_blobs = layers_[i]->blobs();
    CHECK_EQ(target_blobs.size(), source_layer.blobs_size()) << "Incompatible number of blobs for layer " << source_layer_name;
    
    for (int j = 0; j < target_blobs.size(); ++j)
    {
			if (source_layer.blobs(j).has_shape())
			{
				if (source_layer.blobs(j).shape().dim_size()==1)
					CHECK_EQ(target_blobs[j]->num(), source_layer.blobs(j).shape().dim(0));
				else if (source_layer.blobs(j).shape().dim_size()==2)
				{
					CHECK_EQ(target_blobs[j]->num(), source_layer.blobs(j).shape().dim(0));
					CHECK_EQ(target_blobs[j]->channels(), source_layer.blobs(j).shape().dim(1));
				}
				else if (source_layer.blobs(j).shape().dim_size()==3)
				{
					CHECK_EQ(target_blobs[j]->num(), source_layer.blobs(j).shape().dim(0));
					CHECK_EQ(target_blobs[j]->channels(), source_layer.blobs(j).shape().dim(1));
					CHECK_EQ(target_blobs[j]->height(), source_layer.blobs(j).shape().dim(2));
				}
				else if (source_layer.blobs(j).shape().dim_size()==4)
				{
					CHECK_EQ(target_blobs[j]->num(), source_layer.blobs(j).shape().dim(0));
					CHECK_EQ(target_blobs[j]->channels(), source_layer.blobs(j).shape().dim(1));
					CHECK_EQ(target_blobs[j]->height(), source_layer.blobs(j).shape().dim(2));
					CHECK_EQ(target_blobs[j]->width(), source_layer.blobs(j).shape().dim(3));
				}

			}
			else
			{
				CHECK_EQ(target_blobs[j]->num(), source_layer.blobs(j).num());
				CHECK_EQ(target_blobs[j]->channels(), source_layer.blobs(j).channels());
				CHECK_EQ(target_blobs[j]->height(), source_layer.blobs(j).height());
				CHECK_EQ(target_blobs[j]->width(), source_layer.blobs(j).width());
			}
      target_blobs[j]->FromProto(source_layer.blobs(j));
    }
  }
  for (int i = 0; i < param.layer_size(); ++i) {
    if (!layer_used_flag[i] && param.layer(i).blobs_size()) {
      LOG(INFO) << "Ignore source layer " << param.layer(i).name();
    }
 	}	
}
//------------------------------------------------------------------------------------------
//------------------------------------------------------
template <typename Dtype>
void Net<Dtype>::StateToProto(NetState* state) 
{
	state->Clear();
	state->set_adam_iter(adam_iter_);
  state->clear_layer();
  for (int i = 0; i < layers_.size(); i++)
  {
  	LayerParameter* the_layer = state->add_layer();
		for (int j = 0;j < layers_[i]->first_moment().size();j++)
		{
		  BlobProto* first_blob = the_layer->add_first_moment();
		  layers_[i]->first_moment()[j]->ToProto(first_blob);
		  
		  BlobProto* second_blob = the_layer->add_second_moment();
		  layers_[i]->second_moment()[j]->ToProto(second_blob);
		} 
	}	
}
template <typename Dtype>
void Net<Dtype>::RestoreState(const NetState state) 
{

	adam_iter_ = state.adam_iter();
	for (int i = 0; i < layers_.size(); i++)
	{
		//LOG(INFO)<<layer_names_[i]<<", "<<layers_[i]->first_moment().size();
		for (int j = 0;j < layers_[i]->first_moment().size();j++)
		{
			layers_[i]->first_moment()[j]->FromProto(state.layer(i).first_moment(j));
			layers_[i]->second_moment()[j]->FromProto(state.layer(i).second_moment(j));
		}
	}	
}
//------------------------------------------------------

template <typename Dtype>
Net<Dtype>::~Net()
{
}


INSTANTIATE_CLASS(Net);

}  // namespace caffe
