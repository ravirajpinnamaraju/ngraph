// ----------------------------------------------------------------------------
// Copyright 2017 Nervana Systems Inc.
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
// ----------------------------------------------------------------------------

#include "cpu_layout.hpp"
#include "ngraph/descriptor/output.hpp"
#include "ngraph/runtime/cpu/cpu_layout_descriptor.hpp"

using namespace ngraph::runtime::cpu::pass;

bool CPULayout::run_on_call_graph(const std::list<std::shared_ptr<Node>>& nodes)
{
    for (const auto& node : nodes)
    {
        for (size_t i = 0; i < node->get_output_size(); ++i)
        {
            auto tv = node->get_output_tensor_view(i);
            if (tv->get_tensor_view_layout())
            {
                continue;
            }

            auto tvt = tv.get_tensor_view_type();
            auto rank = tvt->get_shape().size();


            auto layout = std::make_shared<ngraph::runtime::cpu::LayoutDescriptor>(*tv);
            tv->set_tensor_view_layout(layout);
        }
    }

    return false;
}
