// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// NVIDIA CORPORATION and its licensors retain all intellectual property
// and proprietary rights in and to this software, related documentation
// and any modifications thereto.  Any use, reproduction, disclosure or
// distribution of this software and related documentation without an express
// license agreement from NVIDIA CORPORATION is strictly prohibited.
//
#pragma once

#include <carb/Interface.h>

namespace aerosim
{
namespace connector
{

/**
 * Interface used to interact with the example C++ USD plugin from Python.
 */
class IAerosimConnector
{
public:
    /// @private
    CARB_PLUGIN_INTERFACE("aerosim::connector::IAerosimConnector", 1, 0);

    /**
     * Creates some example prims using C++.
     */
    virtual void initializeSceneGraph() = 0;

    virtual void onUpdateEvent() = 0;

    /**
     * Remove the example prims using C++.
     */
    virtual void removePrims() = 0;

    /**
     * Print some info about the currently open USD stage from C++.
     */
    virtual void printStageInfo() const = 0;

    /**
     * Called when the default USD stage (ie. the one open in the main viewport) changes.
     * Necessary for now until the omni.usd C++ API becomes ready for public consumption.
     *
     * @param stageId The id of the new default USD stage.
     */
    virtual void onDefaultUsdStageChanged(long stageId) = 0;

    /**
     * Check if an orchestrator stop command has been received.
     *
     * @return true if a stop command was received and not yet cleared.
     */
    virtual bool isStopCommandReceived() = 0;

    /**
     * Clear the stop command received flag after handling it.
     */
    virtual void clearStopCommandReceived() = 0;
};

}
}