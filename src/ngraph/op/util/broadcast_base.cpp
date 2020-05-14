//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include "broadcast_base.hpp"
#include "ngraph/attribute_visitor.hpp"
#include "ngraph/op/concat.hpp"
#include "ngraph/op/constant.hpp"
#include "ngraph/op/sum.hpp"
#include "ngraph/partial_shape.hpp"

#include "ngraph/runtime/reference/broadcast.hpp"

#include <numeric>

using namespace std;
using namespace ngraph;

op::util::BroadcastBase::BroadcastBase(const Output<Node>& arg,
                                       const Output<Node>& target_shape,
                                       const Output<Node>& axes_mapping,
                                       const BroadcastModeSpec& broadcast_mode)
    : Op({arg, target_shape, axes_mapping})
    , m_mode{broadcast_mode}
{
}

op::util::BroadcastBase::BroadcastBase(const Output<Node>& arg,
                                       const Output<Node>& target_shape,
                                       const BroadcastModeSpec& broadcast_mode)
    : Op({arg, target_shape})
    , m_mode{broadcast_mode}
{
}

void op::util::BroadcastBase::validate_and_infer_types()
{
    // shape node should have integer data type. For now we only allow i64
    auto shape_et = get_input_element_type(1);
    NODE_VALIDATION_CHECK(this,
                          shape_et.is_integral_number(),
                          "Broadcast shape must be an integral number, but is: ",
                          shape_et);
    // shape node should produce a one dimensional shape.
    auto broadcast_shape_rank = get_input_partial_shape(1).rank();
    NODE_VALIDATION_CHECK(this,
                          broadcast_shape_rank.compatible(1),
                          "Broadcast shape rank must be 1, but has ",
                          broadcast_shape_rank);

    if (m_mode.m_type == BroadcastType::NONE)
    {
        // axes_mapping node should have integer data type. For now we only allow i64
        auto axes_et = get_input_element_type(2);
        NODE_VALIDATION_CHECK(this,
                              axes_et.is_integral_number(),
                              "Broadcast axes must be integral numbers, but are: ",
                              axes_et);
        // axes_mapping node should produce a one dimensional shape.
        auto axes_shape_rank = get_input_partial_shape(2).rank();
        NODE_VALIDATION_CHECK(this,
                              axes_shape_rank.compatible(1),
                              "Broadcast axes rank must be 1, but has ",
                              axes_shape_rank);
    }

    PartialShape result_shape{PartialShape::dynamic()};

    const auto shape_constant = as_type_ptr<op::v0::Constant>(input_value(1).get_node_shared_ptr());

    if (shape_constant)
    {
        result_shape = shape_constant->get_shape_val();
    }
    else if (auto concat = as_type_ptr<op::v0::Concat>(input_value(1).get_node_shared_ptr()))
    {
        auto concat_inputs = concat->inputs();

        if (concat->get_output_partial_shape(0).is_static() && concat->get_shape().size() == 1 &&
            concat_inputs.size() == shape_size(concat->get_shape()))
        {
            auto output_partial_shape = vector<Dimension>{};
            for (const auto& concat_input : concat_inputs)
            {
                auto source_node_ptr = concat_input.get_source_output().get_node_shared_ptr();
                if (auto source_const_ptr = as_type_ptr<op::v0::Constant>(source_node_ptr))
                {
                    output_partial_shape.push_back(source_const_ptr->get_axis_vector_val()[0]);
                }
                else
                {
                    output_partial_shape.push_back(Dimension::dynamic());
                }
            }
            result_shape = PartialShape(output_partial_shape);
        }
    }

    if (m_mode.m_type == BroadcastType::NONE)
    {
        // Validate axes_mapping
        if (get_input_partial_shape(0).is_static() && get_input_partial_shape(1).is_static() &&
            get_input_partial_shape(2).is_static())
        {
            auto arg_shape = get_input_shape(0);
            auto axes_shape = get_input_shape(2);

            // Rank(arg_shape) == shape_size(axes_mapping)
            NODE_VALIDATION_CHECK(this,
                                  shape_size(axes_shape) == arg_shape.size(),
                                  "Broadcast axes_mapping shape ",
                                  axes_shape,
                                  " doesn't match rank of input tensor ",
                                  arg_shape.size());

            if (shape_constant && input_value(2).get_node_shared_ptr()->is_constant())
            {
                auto target_shape = shape_constant->get_shape_val();
                auto axes_mapping_val =
                    as_type_ptr<op::v0::Constant>(input_value(2).get_node_shared_ptr())
                        ->get_axis_vector_val();
                // axes_mapping needs to be in sorted order
                NODE_VALIDATION_CHECK(
                    this,
                    std::is_sorted(axes_mapping_val.begin(), axes_mapping_val.end()),
                    "Broadcast doesn't permit transposes. axes_mapping ",
                    axes_mapping_val,
                    " not in sorted order");

                for (size_t i = 0; i < axes_mapping_val.size(); i++)
                {
                    NODE_VALIDATION_CHECK(this,
                                          axes_mapping_val[i] < target_shape.size(),
                                          "Broadcast axes_mapping[",
                                          i,
                                          "]: ",
                                          axes_mapping_val[i],
                                          " exceeds target rank ",
                                          target_shape.size());

                    NODE_VALIDATION_CHECK(this,
                                          target_shape[axes_mapping_val[i]] == arg_shape[i],
                                          "Broadcast target[axes_mapping[",
                                          i,
                                          "]]",
                                          " Expected ",
                                          arg_shape[i],
                                          ". Got ",
                                          target_shape[axes_mapping_val[i]]);
                }
            }
        }
    }
    else if (m_mode.m_type == BroadcastType::NUMPY || m_mode.m_type == BroadcastType::PDPD)
    {
        if (get_input_partial_shape(0).is_static() && get_input_partial_shape(1).is_static())
        {
            auto arg_shape = get_input_shape(0);

            if (shape_constant)
            {
                const auto target_shape = shape_constant->get_shape_val();
                auto start_axis = (m_mode.m_type == BroadcastType::PDPD)
                                      ? m_mode.m_axis
                                      : target_shape.size() - arg_shape.size();
                NODE_VALIDATION_CHECK(this,
                                      start_axis >= 0,
                                      "Broadcast target_shape has smaller rank ",
                                      target_shape.size(),
                                      " than arg shape ",
                                      arg_shape.size());
                for (auto i = start_axis; i < target_shape.size(); i++)
                {
                    NODE_VALIDATION_CHECK(
                        this,
                        arg_shape[i - start_axis] == 1 || target_shape[i] == 1 ||
                            arg_shape[i - start_axis] == target_shape[i],
                        "Broadcast incorrect target shape. Expecting either 1 or ",
                        arg_shape[i - start_axis],
                        " . Got ",
                        target_shape[i]);
                    result_shape[i] = std::max(arg_shape[i - start_axis], target_shape[i]);
                }
            }
        }
    }
    set_output_type(0, get_input_element_type(0), result_shape);
}

std::pair<bool, AxisSet> op::util::BroadcastBase::get_broadcast_axes() const
{
    AxisSet broadcast_axes;
    bool axes_known = false;

    if (m_mode.m_type == BroadcastType::NONE)
    {
        const auto axes_mapping_constant =
            as_type_ptr<op::v0::Constant>(input_value(2).get_node_shared_ptr());
        if (get_input_partial_shape(1).is_static() && axes_mapping_constant)
        {
            auto target_shape = get_input_shape(1);
            NGRAPH_CHECK(target_shape.size() == 1);
            auto axes_mapping_val = axes_mapping_constant->get_axis_vector_val();

            std::vector<size_t> axes(target_shape[0]);
            std::iota(axes.begin(), axes.end(), 0);
            for (auto i = axes_mapping_val.rbegin(); i != axes_mapping_val.rend(); ++i)
            {
                axes.erase(axes.begin() + *i);
            }
            broadcast_axes.insert(axes.begin(), axes.end());
            axes_known = true;
        }
    }
    else if (m_mode.m_type == BroadcastType::NUMPY || m_mode.m_type == BroadcastType::PDPD)
    {
        if (get_input_partial_shape(0).is_static() && get_output_partial_shape(0).is_static())
        {
            auto arg_shape = get_input_shape(0);
            auto result_shape = get_output_shape(0);
            auto start_axis = (m_mode.m_type == BroadcastType::PDPD)
                                  ? m_mode.m_axis
                                  : result_shape.size() - arg_shape.size();
            NGRAPH_CHECK(start_axis >= 0);
            for (size_t i = 0; i < result_shape.size(); i++)
            {
                if (i < start_axis || result_shape[i] != arg_shape[i - start_axis])
                {
                    broadcast_axes.insert(i);
                }
            }
            axes_known = true;
        }
    }
    else
    {
        throw ngraph_error("Unknown autobroadcast type");
    }

    return std::make_pair(axes_known, broadcast_axes);
}

void op::util::BroadcastBase::generate_adjoints(autodiff::Adjoints& adjoints,
                                                const OutputVector& deltas)
{
    auto delta = deltas.at(0);

    auto x = input_value(0);

    auto broadcast_axes = get_broadcast_axes();
    if (broadcast_axes.first)
    {
        adjoints.add_delta(x, make_shared<op::Sum>(delta, broadcast_axes.second));
    }
    else
    {
        throw ngraph_error("Autodiff not supported on dynamic op variants");
    }
}

namespace
{
    template <element::Type_t ET>
    inline bool
        evaluate(const HostTensorPtr& arg0, const HostTensorPtr& out, const AxisSet& broadcast_axes)
    {
        using T = typename element_type_traits<ET>::value_type;
        runtime::reference::broadcast<T>((arg0->get_data_ptr<ET>()),
                                         (out->get_data_ptr<ET>()),
                                         arg0->get_shape(),
                                         out->get_shape(),
                                         broadcast_axes);
        return true;
    }

    bool evaluate_broadcast(const HostTensorPtr& arg0,
                            const HostTensorPtr& out,
                            const std::pair<bool, AxisSet> pair_broadcast_axes,
                            const Shape output_shape)
    {
        if (!pair_broadcast_axes.first)
        {
            // broadcast_axes not known deterministically
            return false;
        }
        bool rc = true;
        Shape in_shape = arg0->get_shape();
        out->set_shape(output_shape);
        switch (arg0->get_element_type())
        {
            TYPE_CASE(boolean)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(i8)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(i16)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(i32)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(i64)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(u8)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(u16)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(u32)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(u64)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(bf16)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(f16)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(f32)(arg0, out, pair_broadcast_axes.second);
            break;
            TYPE_CASE(f64)(arg0, out, pair_broadcast_axes.second);
            break;
        default: rc = false; break;
        }
        return rc;
    }
}

bool op::util::BroadcastBase::evaluate(const HostTensorVector& outputs,
                                       const HostTensorVector& inputs)
{
    return evaluate_broadcast(inputs[0], outputs[0], get_broadcast_axes(), get_output_shape(0));
}
