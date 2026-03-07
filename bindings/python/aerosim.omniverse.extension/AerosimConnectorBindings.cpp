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

extern "C" {
    #include "aerosim_world_link.h"
}

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

    // Expose publish_image_to_topic_async for camera sensor image publishing.
    // Accepts a numpy array (or any Python buffer) and passes the raw data
    // to the aerosim-world-link C FFI for JPEG compression and middleware publishing.
    m.def("publish_image_to_topic",
        [](const std::string& topic, int32_t width, int32_t height,
           int32_t format, pybind11::buffer data) {
            pybind11::buffer_info info = data.request();
            ::publish_image_to_topic_async(
                topic.c_str(), width, height, format,
                info.ptr,
                static_cast<uintptr_t>(info.size * info.itemsize));
        },
        pybind11::arg("topic"),
        pybind11::arg("width"),
        pybind11::arg("height"),
        pybind11::arg("format"),
        pybind11::arg("data"),
        "Publish a rendered image to an AeroSim middleware topic.\n"
        "Args:\n"
        "    topic: Middleware topic name (e.g. 'aerosim.renderer.responses')\n"
        "    width: Image width in pixels\n"
        "    height: Image height in pixels\n"
        "    format: Image encoding (0=RGB8, 1=RGBA8, 2=BGR8, 3=BGRA8)\n"
        "    data: Raw pixel data as a numpy array or bytes buffer"
    );
}
}
