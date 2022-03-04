// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/common_optimizations/fq_mul_fusion.hpp"

#include <memory>
#include <ngraph/opsets/opset4.hpp>
#include <ngraph/pattern/op/wrap_type.hpp>
#include <ngraph/rt_info.hpp>
#include <ngraph/validation_util.hpp>
#include <vector>

#include "itt.hpp"
#include "transformations/utils/utils.hpp"

NGRAPH_RTTI_DEFINITION(ngraph::pass::FakeQuantizeMulFusion, "FakeQuantizeMulFusion", 0);

// This transformation multiplies the "output_low" and "output_high" inputs of the FQ operation
// by the constant value that before transormation is used to multiply the output of FQ.
// Both output_low and output_high are multiplied by the value represented as C (a constant) below.
// In case any of the FQ inputs (out_L, out_H) is constant, it gets constant folded with C.
//
//          data  in_L in_H out_L out_H
//            |    |    |     |     |
//            |    |    |     |     |                data  in_L in_H  out_L * C  out_H * C
//            v    v    v     v     v                  |    |    |        |          |
//          +-------------------------+                |    |    |        |          |
//          |       FakeQuantize      |                v    v    v        v          v
//          +-------------------------+             +-----------------------------------+
//                       |                =====>    |            FakeQuantize           |
//                       v                          +-----------------------------------+
//                  +----------+                                      |
//                  | Multiply | <--- C                               v
//                  +----+-----+
//                       |
//                       v
//

ngraph::pass::FakeQuantizeMulFusion::FakeQuantizeMulFusion() {
    MATCHER_SCOPE(FakeQuantizeMulFusion);
    const auto data_p = ngraph::pattern::any_input();
    const auto fq_output_low_p = ngraph::pattern::any_input();
    const auto fq_output_high_p = ngraph::pattern::any_input();

    const auto fq_node_p = ngraph::pattern::wrap_type<opset4::FakeQuantize>(
        {data_p, ngraph::pattern::any_input(), ngraph::pattern::any_input(), fq_output_low_p, fq_output_high_p},
        pattern::consumers_count(1));

    const auto mul_constant_p = ngraph::pattern::wrap_type<opset4::Constant>();
    const auto mul_node_p =
        ngraph::pattern::wrap_type<opset4::Multiply>({fq_node_p, mul_constant_p}, pattern::consumers_count(1));

    ngraph::matcher_pass_callback callback = [=](pattern::Matcher& m) {
        const auto& pattern_map = m.get_pattern_value_map();

        const auto& data = pattern_map.at(data_p);
        const auto fq_node = pattern_map.at(fq_node_p).get_node_shared_ptr();

        const auto& original_output_low = pattern_map.at(fq_output_low_p);
        const auto& original_output_high = pattern_map.at(fq_output_high_p);
        auto mul_constant = pattern_map.at(mul_constant_p).get_node_shared_ptr();
        auto mul_constant_shape = mul_constant->get_shape();
        bool is_single_value = shape_size(mul_constant_shape) == 1;

        if (!is_single_value) {
            float v;
            auto constant = std::dynamic_pointer_cast<opset4::Constant>(mul_constant);
            if (constant) {
                is_single_value = op::util::get_single_value(constant, v);
                if (is_single_value) {
                    mul_constant_shape = Shape{1};
                    mul_constant =
                        std::make_shared<opset4::Constant>(mul_constant->get_element_type(), mul_constant_shape, v);
                }
            }
        }

        if (!is_single_value) {
            auto fq_outputs = fq_node->get_users();
            // Convolution and GroupConvolution LP transformations require output low/high to have the same values
            bool fq_output_is_conv =
                std::any_of(fq_outputs.begin(), fq_outputs.end(), [](const std::shared_ptr<Node>& node) -> bool {
                    return is_type<opset4::Convolution>(node) || is_type<opset4::GroupConvolution>(node);
                });
            if (fq_output_is_conv) {
                return false;
            }
            const auto& data_rank = data.get_partial_shape().rank();
            if (data_rank.is_dynamic()) {
                return false;
            }
            auto rank = data_rank.get_length();
            auto diff = rank - mul_constant_shape.size();
            if (diff > 0) {
                mul_constant_shape.insert(mul_constant_shape.begin(), diff, 1);
                mul_constant = std::make_shared<ngraph::opset4::Reshape>(
                    mul_constant,
                    op::Constant::create(element::i64, Shape{mul_constant_shape.size()}, mul_constant_shape),
                    false);
            }
        }

        auto get_adjusted_output_range = [&](const Output<Node>& node) -> std::shared_ptr<Node> {
            auto ret = std::make_shared<ngraph::opset4::Multiply>(node, mul_constant);
            copy_runtime_info(node.get_node_shared_ptr(), ret);
            auto constant = get_constant_from_source(ret);
            if (constant)
                return constant;
            return ret;
        };

        const auto new_fq_node = fq_node->clone_with_new_inputs({fq_node->input_value(0),
                                                                 fq_node->input_value(1),
                                                                 fq_node->input_value(2),
                                                                 get_adjusted_output_range(original_output_low),
                                                                 get_adjusted_output_range(original_output_high)});
        bool fq_on_weights = is_type<opset4::Constant>(data.get_node());
        if (!fq_on_weights && transformation_callback(new_fq_node))
            return false;

        const auto mul_node = pattern_map.at(mul_node_p).get_node_shared_ptr();

        // WA: this check is intended to prevent replacement when new FQ has shape
        // which is different to Multiply output shape. Otherwise such replacement
        // will lead to shape inconsistency in remaining graph. This check must be
        // removed in future when FQ will have correct validate_and_infer function
        // for cases with NUMPY broadcast.
        auto fq_casted = std::dynamic_pointer_cast<opset4::FakeQuantize>(new_fq_node);
        if (!fq_casted) {
            return false;
        }
        if (fq_casted->get_auto_broadcast() == op::AutoBroadcastType::NUMPY) {
            if (fq_casted->get_output_partial_shape(0).is_dynamic() ||
                mul_node->get_output_partial_shape(0).is_dynamic()) {
                return false;
            }
            if (fq_casted->get_shape() != mul_node->get_shape()) {
                return false;
            }
        }

        replace_node(mul_node, new_fq_node);

        new_fq_node->set_friendly_name(mul_node->get_friendly_name());
        copy_runtime_info({fq_node, mul_node}, new_fq_node);

        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(mul_node_p, matcher_name);
    this->register_matcher(m, callback);
}
