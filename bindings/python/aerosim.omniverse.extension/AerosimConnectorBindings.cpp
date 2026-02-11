// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// NVIDIA CORPORATION and its licensors retain all intellectual property
// and proprietary rights in and to this software, related documentation
// and any modifications thereto.  Any use, reproduction, disclosure or
// distribution of this software and related documentation without an express
// license agreement from NVIDIA CORPORATION is strictly prohibited.
//

#include <carb/BindingsPythonUtils.h>

#include <aerosim/omniverse/extension/IAerosimConnector.h>

CARB_BINDINGS("aerosim.omniverse.extension.python")

DISABLE_PYBIND11_DYNAMIC_CAST(aerosim::connector::IAerosimConnector)

namespace
{

// Define the pybind11 module using the same name specified in premake5.lua
PYBIND11_MODULE(_aerosim_connector_bindings, m)
{
    using namespace aerosim::connector;

    m.doc() = "pybind11 aerosim.omniverse.extension bindings";

    carb::defineInterfaceClass<IAerosimConnector>(
        m, "IAerosimConnector", "acquire_aerosim_connector", "release_aerosim_connector")
        .def("initialize_scene_graph", &IAerosimConnector::initializeSceneGraph)
        .def("remove_prims", &IAerosimConnector::removePrims)
        .def("print_stage_info", &IAerosimConnector::printStageInfo)
        .def("on_default_usd_stage_changed", &IAerosimConnector::onDefaultUsdStageChanged)
        .def("on_update_event", &IAerosimConnector::onUpdateEvent)
        .def("is_stop_command_received", &IAerosimConnector::isStopCommandReceived)
        .def("clear_stop_command_received", &IAerosimConnector::clearStopCommandReceived)
    /**/;
}
}
