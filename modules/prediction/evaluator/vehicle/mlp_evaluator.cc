/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include <cmath>
#include <fstream>
#include <numeric>

#include "modules/prediction/evaluator/vehicle/mlp_evaluator.h"
#include "modules/prediction/common/prediction_gflags.h"
#include "modules/common/math/math_utils.h"
#include "modules/prediction/common/prediction_util.h"

namespace apollo {
namespace prediction {

void MLPEvaluator::Clear() {
  obstacle_feature_values_map_.clear();
}

void MLPEvaluator::Evaluate(Obstacle* obstacle_ptr) {
  Clear();
  if (obstacle_ptr == nullptr) {
    AERROR << "Invalid obstacle.";
    return;
  }

  int id = obstacle_ptr->id();
  Feature latest_feature = obstacle_ptr->latest_feature();
  if (!latest_feature.IsInitialized()) {
    ADEBUG << "Obstacle [" << id << "] has no latest feature.";
    return;
  }

  Lane* lane_ptr = latest_feature.mutable_lane();
  if (!latest_feature.has_lane() || lane_ptr == nullptr) {
    ADEBUG << "Obstacle [" << id << "] has no lane feature.";
    return;
  }

  LaneGraph* lane_graph_ptr = lane_ptr->mutable_lane_graph();
  if (!latest_feature.lane().has_lane_graph() || lane_graph_ptr == nullptr) {
    ADEBUG << "Obstacle [" << id << "] has no lane graph.";
    return;
  }

  if (latest_feature.lane().lane_graph().lane_sequence_size() == 0) {
    ADEBUG << "Obstacle [" << id << "] has no lane sequences.";
    return;
  }

  for (int i = 0;
      i < latest_feature.lane().lane_graph().lane_sequence_size(); ++i) {
    LaneSequence* lane_sequence_ptr = lane_graph_ptr->mutable_lane_sequence(i);
    CHECK(lane_sequence_ptr != nullptr);
    ExtractFeatureValues(obstacle_ptr, lane_sequence_ptr);
  }
}

void MLPEvaluator::ExtractFeatureValues(Obstacle* obstacle_ptr,
                                        LaneSequence* lane_sequence_ptr) {
  feature_values_.clear();
  int id = obstacle_ptr->id();
  std::vector<double> obstacle_feature_values;
  if (obstacle_feature_values_map_.find(id) ==
      obstacle_feature_values_map_.end()) {
    SetObstacleFeatureValues(obstacle_ptr, &obstacle_feature_values);
  } else {
    obstacle_feature_values = obstacle_feature_values_map_[id];
  }

  if (obstacle_feature_values.size() != OBSTACLE_FEATURE_SIZE) {
    ADEBUG << "Obstacle [" << id << "] has fewer than "
           << "expected obstacle feature_values "
           << obstacle_feature_values.size() << ".";
    return;
  }

  std::vector<double> lane_feature_values;
  SetLaneFeatureValues(obstacle_ptr, lane_sequence_ptr, &lane_feature_values);
  if (lane_feature_values.size() != LANE_FEATURE_SIZE) {
    ADEBUG << "Obstacle [" << id << "] has fewer than "
           << "expected lane feature_values"
           << lane_feature_values.size() << ".";
    return;
  }

  feature_values_.insert(feature_values_.end(),
      lane_feature_values.begin(), lane_feature_values.end());
  feature_values_.insert(feature_values_.end(),
      lane_feature_values.begin(), lane_feature_values.end());
}

void MLPEvaluator::SetObstacleFeatureValues(
    Obstacle* obstacle_ptr, std::vector<double>* feature_values) {
  feature_values->clear();
  feature_values->reserve(OBSTACLE_FEATURE_SIZE);

  std::vector<double> thetas;
  std::vector<double> lane_ls;
  std::vector<double> dist_lbs;
  std::vector<double> dist_rbs;
  std::vector<int> lane_types;
  std::vector<double> speeds;
  std::vector<double> timestamps;

  double duration = obstacle_ptr->timestamp() - FLAGS_prediction_duration;
  int count = 0;
  for (std::size_t i = 0; i < obstacle_ptr->history_size(); ++i) {
    const Feature& feature = obstacle_ptr->feature(i);
    if (!feature.IsInitialized()) {
      continue;
    }
    if (apollo::common::math::DoubleCompare(
            feature.timestamp(), duration) < 0) {
      break;
    }
    if (feature.has_lane() && feature.lane().has_lane_feature()) {
      thetas.push_back(feature.lane().lane_feature().angle_diff());
      lane_ls.push_back(feature.lane().lane_feature().lane_l());
      dist_lbs.push_back(feature.lane().lane_feature().dist_to_left_boundary());
      dist_rbs.push_back(
          feature.lane().lane_feature().dist_to_right_boundary());
      lane_types.push_back(feature.lane().lane_feature().lane_turn_type());
      timestamps.push_back(feature.timestamp());
      if (FLAGS_enable_kf_tracking) {
        speeds.push_back(feature.t_speed());
      } else {
        speeds.push_back(feature.speed());
      }
      ++count;
    }
  }
  if (count <= 0) {
    return;
  }
  double theta_mean =
      std::accumulate(thetas.begin(), thetas.end(), 0.0) / thetas.size();
  double theta_filtered =
      (thetas.size() > 1) ? (thetas[0] + thetas[1]) / 2.0 : thetas[0];
  double lane_l_mean =
      std::accumulate(lane_ls.begin(), lane_ls.end(), 0.0) / lane_ls.size();
  double lane_l_filtered =
      (lane_ls.size() > 1) ? (lane_ls[0] + lane_ls[1]) / 2.0 : lane_ls[0];
  double speed_mean =
      std::accumulate(speeds.begin(), speeds.end(), 0.0) / speeds.size();
  double speed_lateral = sin(theta_filtered) * speed_mean;
  double speed_sign = (speed_lateral > 0) ? 1.0 : -1.0;
  double time_to_lb = (abs(speed_lateral) > 0.05)
                          ? dist_lbs[0] / speed_lateral
                          : 20 * dist_lbs[0] * speed_sign;
  double time_to_rb = (abs(speed_lateral) > 0.05)
                          ? -1 * dist_rbs[0] / speed_lateral
                          : -20 * dist_rbs[0] * speed_sign;
  double time_diff = timestamps.front() - timestamps.back();
  double dist_lb_rate = (timestamps.size() > 1)
                            ? (dist_lbs.front() - dist_lbs.back()) / time_diff
                            : 0.0;
  double dist_rb_rate = (timestamps.size() > 1)
                            ? (dist_rbs.front() - dist_rbs.back()) / time_diff
                            : 0.0;
  // setup obstacle feature values
  feature_values->push_back(theta_filtered);
  feature_values->push_back(theta_mean);
  feature_values->push_back(theta_filtered - theta_mean);
  feature_values->push_back((thetas.size() > 1) ? thetas[0] - thetas[1]
                                            : thetas[0]);
  feature_values->push_back(lane_l_filtered);
  feature_values->push_back(lane_l_mean);
  feature_values->push_back(lane_l_filtered - lane_l_mean);
  feature_values->push_back(speed_mean);
  feature_values->push_back(dist_lbs.front());
  feature_values->push_back(dist_lb_rate);
  feature_values->push_back(time_to_lb);
  feature_values->push_back(dist_rbs.front());
  feature_values->push_back(dist_rb_rate);
  feature_values->push_back(time_to_rb);
  // feature_values->push_back(lane_types.front() == HDMapLane::NO_TURN ? 1.0
  //                                                                    : 0.0);
  // feature_values->push_back(lane_types.front() == HDMapLane::LEFT_TURN ? 1.0
  //                                                                     : 0.0);
  // feature_values->push_back(lane_types.front() == HDMapLane::RIGHT_TURN ? 1.0
  //                                                                   : 0.0);
  // feature_values->push_back(lane_types.front() == HDMapLane::U_TURN ? 1.0
  //                                                                   : 0.0);
}

void MLPEvaluator::SetLaneFeatureValues(Obstacle* obstacle_ptr,
    LaneSequence* lane_sequence_ptr, std::vector<double>* feature_values) {
  feature_values->clear();
  feature_values->reserve(LANE_FEATURE_SIZE);
  const Feature& feature = obstacle_ptr->latest_feature();
  if (!feature.IsInitialized()) {
    ADEBUG << "Obstacle [" << obstacle_ptr->id() << "] has no latest feature.";
    return;
  } else if (!feature.has_position()) {
    ADEBUG << "Obstacle [" << obstacle_ptr->id() << "] has no position.";
    return;
  }

  double heading = FLAGS_enable_kf_tracking ? feature.t_velocity_heading()
                                            : feature.theta();
  for (int i = 0; i < lane_sequence_ptr->lane_segment_size(); ++i) {
    if (feature_values->size() >= LANE_FEATURE_SIZE) {
      break;
    }
    const LaneSegment& lane_segment = lane_sequence_ptr->lane_segment(i);
    for (int j = 0; j < lane_segment.lane_point_size(); ++j) {
      if (feature_values->size() >= LANE_FEATURE_SIZE) {
        break;
      }
      const LanePoint& lane_point = lane_segment.lane_point(j);
      if (!lane_point.has_position()) {
        AERROR << "Lane point has no position.";
        continue;
      }
      double diff_x = lane_point.position().x() - feature.position().x();
      double diff_y = lane_point.position().y() - feature.position().y();
      double angle = std::atan2(diff_x, diff_y);
      feature_values->push_back(std::sin(angle - heading));
      feature_values->push_back(lane_point.relative_l());
      feature_values->push_back(lane_point.heading());
      feature_values->push_back(lane_point.angle_diff());
    }
  }

  std::size_t size = feature_values->size();
  while (size >= 4 && size < LANE_FEATURE_SIZE) {
    double heading_diff = feature_values->operator[](size - 4);
    double lane_l_diff = feature_values->operator[](size - 3);
    double heading = feature_values->operator[](size - 2);
    double angle_diff = feature_values->operator[](size - 1);
    feature_values->push_back(heading_diff);
    feature_values->push_back(lane_l_diff);
    feature_values->push_back(heading);
    feature_values->push_back(angle_diff);
    size = feature_values->size();
  }
}

void MLPEvaluator::LoadModel(const std::string& model_file) {
  model_ptr_.reset(new FnnVehicleModel());
  CHECK(model_ptr_ != nullptr);
  std::fstream file_stream(model_file, std::ios::in | std::ios::binary);
  if (!file_stream.good()) {
    AERROR << "Unable to open the model file: " << model_file << ".";
    return;
  }
  if (!model_ptr_->ParseFromIstream(&file_stream)) {
    AERROR << "Unable to load the model file: " << model_file << ".";
    return;
  }
  ADEBUG << "Succeeded in loading the model file: " << model_file << ".";
}

double MLPEvaluator::ComputeProbability() {
  CHECK(model_ptr_.get() != nullptr);
  double probability = 0.0;

  if (model_ptr_->dim_input() != static_cast<int>(feature_values_.size())) {
    AERROR << "Model feature size not consistent with model proto definition.";
    return probability;
  }
  std::vector<double> layer_input;
  layer_input.reserve(model_ptr_->dim_input());
  std::vector<double> layer_output;

  // normalization
  for (int i = 0; i < model_ptr_->dim_input(); ++i) {
    double mean = model_ptr_->samples_mean().columns(i);
    double std = model_ptr_->samples_std().columns(i);
    layer_input.push_back(
        apollo::prediction::util::Normalize(feature_values_[i], mean, std));
  }

  for (int i = 0; i < model_ptr_->num_layer(); ++i) {
    if (i > 0) {
      layer_input.clear();
      layer_output.swap(layer_output);
    }
    const Layer& layer = model_ptr_->layer(i);
    for (int col = 0; col < layer.layer_output_dim(); ++col) {
      double neuron_output = layer.layer_bias().columns(col);
      for (int row = 0; row < layer.layer_input_dim(); ++row) {
        double weight = layer.layer_input_weight().rows(row).columns(col);
        neuron_output += (layer_input[row] * weight);
      }
      if (layer.layer_activation_type() == "relu") {
        neuron_output = apollo::prediction::util::Relu(neuron_output);
      } else if (layer.layer_activation_type() == "sigmoid") {
        neuron_output = apollo::prediction::util::Sigmoid(neuron_output);
      } else if (layer.layer_activation_type() == "tanh") {
        neuron_output = std::tanh(neuron_output);
      } else {
        LOG(ERROR) << "Undefined activation func: "
                   << layer.layer_activation_type()
                   << ", and default sigmoid will be used instead.";
        neuron_output = apollo::prediction::util::Sigmoid(neuron_output);
      }
      layer_output.push_back(neuron_output);
    }
  }

  if (layer_output.size() != 1) {
    AERROR << "Model output layer has incorrect # outputs: "
           << layer_output.size();
  } else {
    probability = layer_output[0];
  }

  return probability;
}

}  // namespace prediction
}  // namespace apollo
