/* ------------------------------------------------ *
 * The MIT License (MIT)
 * Copyright (c) 2020 terryky1220@gmail.com
 * ------------------------------------------------ */

// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "glue_mediapipe.h"
#include <cmath>

static float 
CalculateScale(float min_scale, float max_scale, int stride_index, int num_strides) 
{
    if (num_strides == 1)
        return (min_scale + max_scale) * 0.5f;
    else
        return min_scale + (max_scale - min_scale) * 1.0 * stride_index / (num_strides - 1.0f);
}

int
GenerateAnchors(std::vector<Anchor>* anchors, const SsdAnchorsCalculatorOptions& options)
{
    int layer_id = 0;
    while (layer_id < (int)options.strides.size()) {
        std::vector<float> anchor_height;
        std::vector<float> anchor_width;
        std::vector<float> aspect_ratios;
        std::vector<float> scales;

        // For same strides, we merge the anchors in the same order.
        int last_same_stride_layer = layer_id;
        while (last_same_stride_layer < (int)options.strides.size() &&
               options.strides[last_same_stride_layer] == options.strides[layer_id]) 
        {
          const float scale =
              CalculateScale(options.min_scale, options.max_scale,
                last_same_stride_layer, options.strides.size());
          if (last_same_stride_layer == 0 && options.reduce_boxes_in_lowest_layer) {
            // For first layer, it can be specified to use predefined anchors.
            aspect_ratios.push_back(1.0);
            aspect_ratios.push_back(2.0);
            aspect_ratios.push_back(0.5);
            scales.push_back(0.1);
            scales.push_back(scale);
            scales.push_back(scale);
          } else {
            for (int aspect_ratio_id = 0;
                aspect_ratio_id < (int)options.aspect_ratios.size();
                 ++aspect_ratio_id) {
              aspect_ratios.push_back(options.aspect_ratios[aspect_ratio_id]);
              scales.push_back(scale);
            }
            if (options.interpolated_scale_aspect_ratio > 0.0) {
              const float scale_next =
                last_same_stride_layer == (int)options.strides.size() - 1
                      ? 1.0f
                      : CalculateScale(options.min_scale, options.max_scale,
                                       last_same_stride_layer + 1,
                                       options.strides.size());
              scales.push_back(std::sqrt(scale * scale_next));
              aspect_ratios.push_back(options.interpolated_scale_aspect_ratio);
            }
          }
          last_same_stride_layer++;
        }

        for (int i = 0; i < (int)aspect_ratios.size(); ++i) {
          const float ratio_sqrts = std::sqrt(aspect_ratios[i]);
          anchor_height.push_back(scales[i] / ratio_sqrts);
          anchor_width .push_back(scales[i] * ratio_sqrts);
        }

        int feature_map_height = 0;
        int feature_map_width  = 0;
        if (options.feature_map_height.size()) {
          feature_map_height = options.feature_map_height[layer_id];
          feature_map_width  = options.feature_map_width [layer_id];
        } else {
          const int stride = options.strides[layer_id];
          feature_map_height = std::ceil(1.0f * options.input_size_height / stride);
          feature_map_width  = std::ceil(1.0f * options.input_size_width  / stride);
        }

        for (int y = 0; y < feature_map_height; ++y) {
          for (int x = 0; x < feature_map_width; ++x) {
            for (int anchor_id = 0; anchor_id < (int)anchor_height.size(); ++anchor_id) {
              // TODO: Support specifying anchor_offset_x, anchor_offset_y.
              const float x_center = (x + options.anchor_offset_x) * 1.0f / feature_map_width;
              const float y_center = (y + options.anchor_offset_y) * 1.0f / feature_map_height;

              Anchor new_anchor;
              new_anchor.x_center = x_center;
              new_anchor.y_center = y_center;

              if (options.fixed_anchor_size) {
                new_anchor.w = 1.0f;
                new_anchor.h = 1.0f;
              } else {
                new_anchor.w = anchor_width [anchor_id];
                new_anchor.h = anchor_height[anchor_id];
              }
              anchors->push_back(new_anchor);
            }
          }
        }
        layer_id = last_same_stride_layer;
    }
    return 0;
}



/* -------------------------------------------------- *
 *  Apply NonMaxSuppression:
 *      https://github.com/tensorflow/tfjs/blob/master/tfjs-core/src/ops/image_ops.ts
 * -------------------------------------------------- */
static float
calc_intersection_over_union (detect_region_t &region0, detect_region_t &region1)
{
    float sx0 = region0.topleft.x;
    float sy0 = region0.topleft.y;
    float ex0 = region0.btmright.x;
    float ey0 = region0.btmright.y;
    float sx1 = region1.topleft.x;
    float sy1 = region1.topleft.y;
    float ex1 = region1.btmright.x;
    float ey1 = region1.btmright.y;

    float xmin0 = std::min (sx0, ex0);
    float ymin0 = std::min (sy0, ey0);
    float xmax0 = std::max (sx0, ex0);
    float ymax0 = std::max (sy0, ey0);
    float xmin1 = std::min (sx1, ex1);
    float ymin1 = std::min (sy1, ey1);
    float xmax1 = std::max (sx1, ex1);
    float ymax1 = std::max (sy1, ey1);

    float area0 = (ymax0 - ymin0) * (xmax0 - xmin0);
    float area1 = (ymax1 - ymin1) * (xmax1 - xmin1);
    if (area0 <= 0 || area1 <= 0)
        return 0.0f;

    float intersect_xmin = std::max (xmin0, xmin1);
    float intersect_ymin = std::max (ymin0, ymin1);
    float intersect_xmax = std::min (xmax0, xmax1);
    float intersect_ymax = std::min (ymax0, ymax1);

    float intersect_area = std::max (intersect_ymax - intersect_ymin, 0.0f) *
                           std::max (intersect_xmax - intersect_xmin, 0.0f);

    return intersect_area / (area0 + area1 - intersect_area);
}

static bool
compare (detect_region_t &v1, detect_region_t &v2)
{
    if (v1.score > v2.score)
        return true;
    else
        return false;
}

int
non_max_suppression (std::list<detect_region_t> &region_list, std::list<detect_region_t> &region_nms_list, float iou_thresh)
{
    region_list.sort (compare);

    for (auto itr = region_list.begin(); itr != region_list.end(); itr ++)
    {
        detect_region_t region_candidate = *itr;

        int ignore_candidate = false;
        for (auto itr_nms = region_nms_list.rbegin(); itr_nms != region_nms_list.rend(); itr_nms ++)
        {
            detect_region_t region_nms = *itr_nms;

            float iou = calc_intersection_over_union (region_candidate, region_nms);
            if (iou >= iou_thresh)
            {
                ignore_candidate = true;
                break;
            }
        }

        if (!ignore_candidate)
        {
            region_nms_list.push_back(region_candidate);
            if (region_nms_list.size() >= MAX_POSE_NUM)
                break;
        }
    }

    return 0;
}

