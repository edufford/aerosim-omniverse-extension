## Copyright (c) 2022, NVIDIA CORPORATION.  All rights reserved.
##
## NVIDIA CORPORATION and its licensors retain all intellectual property
## and proprietary rights in and to this software, related documentation
## and any modifications thereto.  Any use, reproduction, disclosure or
## distribution of this software and related documentation without an express
## license agreement from NVIDIA CORPORATION is strictly prohibited.
##
import os
import time
import threading

import carb
import omni.ext
import omni.usd
import omni.ext
import omni.kit.app
import omni.kit.commands

import omni.kit.viewport.utility as viewport_utility

import cesium.omniverse as cesium
from .._aerosim_connector_bindings import *
from pxr import Gf, Sdf, Usd, UsdGeom

# Global public interface object.
_aerosim_connector = None

# Public API.
def get_aerosim_connector() -> IAerosimConnector:
    return _aerosim_connector


# Use the extension entry points to acquire and release the interface,
# and to subscribe to usd stage events.
class AerosimConnector(omni.ext.IExt):
    def on_startup(self):
        # Disable gamepad camera control to prevent conflict with pilot control input
        carb.settings.get_settings().set("persistent/app/omniverse/gamepadCameraControl", False)

        # Acquire the example USD interface.
        global _aerosim_connector
        _aerosim_connector = acquire_aerosim_connector()

        # Inform the C++ plugin if a USD stage is already open.
        usd_context = omni.usd.get_context()
        if usd_context.get_stage_state() == omni.usd.StageState.OPENED:
            _aerosim_connector.on_default_usd_stage_changed(usd_context.get_stage_id())

        # Subscribe to omni.usd stage events so we can inform the C++ plugin when a new stage opens.
        self._stage_event_sub = usd_context.get_stage_event_stream().create_subscription_to_pop(
            self._on_stage_event, name="aerosim.omniverse.extension"
        )

        # Subscribe to update events (called every frame)
        app = omni.kit.app.get_app_interface()
        if app:
            self._update_event_sub = app.get_update_event_stream().create_subscription_to_pop(
                self._update, name="aerosim.omniverse.extension."
            )

    def _update(self, event):
        _aerosim_connector.on_update_event()
        self.update_viewport_camera()

    def on_shutdown(self):
        global _aerosim_connector

        # Remove the example prims from C++.
        _aerosim_connector.remove_prims()

        # Unsubscribe from omni.usd stage events.
        self._stage_event_sub = None

        # Release the example USD interface.
        release_aerosim_connector(_aerosim_connector)
        _aerosim_connector = None

    def _on_stage_event(self, event):
        if event.type == int(omni.usd.StageEventType.OPENED):
            _aerosim_connector.on_default_usd_stage_changed(omni.usd.get_context().get_stage_id())
            _aerosim_connector.initialize_scene_graph()
            _aerosim_connector.print_stage_info()
            print("[AerosimCesiumExtension] Starting Cesium Extension...")
            # Ensure Cesium extension is enabled
            self.enable_cesium_extension()

        elif event.type == int(omni.usd.StageEventType.CLOSED):
            _aerosim_connector.on_default_usd_stage_changed(0)

    def enable_cesium_extension(self):
        """Ensure Cesium for Omniverse is enabled."""
        ext_manager = omni.kit.app.get_app().get_extension_manager()
        if ext_manager:
            ext_manager.set_extension_enabled("cesium.omniverse", True)
            print("[AerosimCesiumExtension] Cesium enabled successfully!")
        else:
            print("[AerosimCesiumExtension] ERROR: Failed to enable Cesium.")

        """Create Cesium World in the Omniverse stage."""
        stage = omni.usd.get_context().get_stage()
        if not stage:
            print("[AerosimCesiumExtension] ERROR: No USD stage found!")
            return

        cesium_token = os.environ.get("AEROSIM_CESIUM_TOKEN")
        if cesium_token:
            print("[AerosimCesiumExtension] Cesium token found")
            cesium.add_tileset_ion("Cesium World Terrain", 2275207, cesium_token)
            self.load_coordinates(33.93651939935984, -118.41269814369221, 0.0)
        else:
            print("[AerosimCesiumExtension] ERROR: Cesium Token not found!")

    def load_coordinates(self, lat, lon, alt):
        """Load coordinates into the Cesium Georeference."""
        print(f"[AerosimCesiumExtension] Loading coordinates: {lat}, {lon}, {alt}")
        CesiumGeoreference = cesium.get_or_create_cesium_georeference()
        CesiumGeoreference.GetPrim().GetAttribute("cesium:georeferenceOrigin:latitude").Set(lat)
        CesiumGeoreference.GetPrim().GetAttribute("cesium:georeferenceOrigin:longitude").Set(lon)
        CesiumGeoreference.GetPrim().GetAttribute("cesium:georeferenceOrigin:height").Set(alt)
        print("[AerosimCesiumExtension] Coordinates loaded successfully!")

    def update_viewport_camera(self):
        usd_context = omni.usd.get_context()
        stage = usd_context.get_stage()
        if not stage:
            # print("[AerosimCesiumExtension] ERROR: No USD stage found!")
            return

        viewport = viewport_utility.get_active_viewport()
        if not viewport:
            return

        prim = stage.GetPrimAtPath("/Resources/viewport_config")
        if not (prim and prim.IsValid()):
            return

        active_camera_rel = prim.GetRelationship("active_camera_ref")
        if not active_camera_rel:
            return

        # Retrieve the list of target paths.
        active_camera_targets = active_camera_rel.GetTargets()
        if not active_camera_targets or len(active_camera_targets) == 0:
            return

        disable_viewport_config = prim.GetAttribute("disable_viewport_config").Get()
        if disable_viewport_config:
            return

        # Use the first target path as the active camera.
        viewport.camera_path = str(active_camera_targets[0])
