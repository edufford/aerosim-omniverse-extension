"""
Camera sensor manager using Isaac Sim Camera class and World simulation context.

Discovers camera sensors from the AeroSim scene graph, creates Isaac Sim Camera
objects added to the World scene, captures RGBA frames via Camera.get_rgba(),
and publishes them to the AeroSim middleware pipeline via publish_image_to_topic().
"""

import math
import numpy as np
import carb

from pxr import Sdf, Usd, UsdGeom

# Image format constants matching aerosim-world-link ImageEncoding enum
IMAGE_FORMAT_RGB8 = 0
IMAGE_FORMAT_RGBA8 = 1
IMAGE_FORMAT_BGR8 = 2
IMAGE_FORMAT_BGRA8 = 3

# Middleware topic for renderer image responses
RENDERER_RESPONSES_TOPIC = "aerosim.renderer.responses"


class CameraSensorInfo:
    """Holds configuration and runtime state for a single camera sensor."""

    def __init__(self, entity_id, sensor_name, camera_prim_path, width, height, fov, tick_rate):
        self.entity_id = entity_id
        self.sensor_name = sensor_name
        self.camera_prim_path = camera_prim_path
        self.width = width
        self.height = height
        self.fov = fov
        self.tick_rate = tick_rate
        self.camera = None  # Isaac Sim Camera instance
        self.initialized = False
        self.frame_count = 0  # Track frames since initialization for warmup


class CameraSensorManager:
    """Manages Isaac Sim Camera sensors for image capture and publishing."""

    def __init__(self):
        self._cameras = {}  # entity_id -> CameraSensorInfo
        self._cameras_initialized = False
        self._publish_fn = None

    def set_publish_function(self, publish_fn):
        """Set the publish_image_to_topic function from the pybind11 bindings."""
        self._publish_fn = publish_fn

    def has_cameras(self):
        """Return True if any camera sensors have been discovered."""
        return len(self._cameras) > 0

    @property
    def cameras_initialized(self):
        return self._cameras_initialized

    def discover_cameras(self, stage):
        """Scan /Components/sensor/* prims for rgb_camera type sensors.

        For each camera sensor found, reads the resolution, tick_rate, and FOV
        from USD attributes. Resolves the camera prim path via the entity's
        actor_ref relationship.

        Args:
            stage: The current USD stage.
        """
        self._cameras.clear()
        self._cameras_initialized = False

        sensor_root = stage.GetPrimAtPath(Sdf.Path("/Components/sensor"))
        if not sensor_root or not sensor_root.IsValid():
            return

        for sensor_prim in sensor_root.GetChildren():
            entity_id = sensor_prim.GetPath().name

            sensor_type_attr = sensor_prim.GetAttribute("sensor:sensor_type")
            if not sensor_type_attr:
                continue

            sensor_type = sensor_type_attr.Get()
            if "rgb_camera" not in sensor_type:
                continue

            # Read sensor parameters
            sensor_name = sensor_prim.GetAttribute("sensor:sensor_name").Get() or entity_id
            resolution = sensor_prim.GetAttribute("sensor:parameters:resolution").Get()
            tick_rate = sensor_prim.GetAttribute("sensor:parameters:tick_rate").Get() or 0.0
            fov = sensor_prim.GetAttribute("sensor:parameters:fov").Get() or 0.0

            width = resolution[0] if resolution else 0
            height = resolution[1] if resolution else 0

            # Check capture_enabled (defaults to false if not set)
            capture_attr = sensor_prim.GetAttribute("sensor:parameters:capture_enabled")
            capture_enabled = capture_attr.Get() if capture_attr else None
            if capture_enabled is None:
                carb.log_warn(
                    f"[CameraSensorManager] capture_enabled not set for {sensor_name}, defaulting to disabled"
                )
                capture_enabled = False

            # Resolve camera prim path via entity -> actor_ref relationship
            entity_prim = stage.GetPrimAtPath(Sdf.Path(f"/Entities/{entity_id}"))
            if not entity_prim or not entity_prim.IsValid():
                carb.log_warn(f"[CameraSensorManager] Entity prim not found: /Entities/{entity_id}")
                continue

            actor_rel = entity_prim.GetRelationship("actor_ref")
            if not actor_rel:
                carb.log_warn(f"[CameraSensorManager] No actor_ref for entity {entity_id}")
                continue

            actor_paths = actor_rel.GetForwardedTargets()
            if not actor_paths:
                carb.log_warn(f"[CameraSensorManager] No actor targets for entity {entity_id}")
                continue

            # Find the UsdGeomCamera child in the actor hierarchy
            actor_prim = stage.GetPrimAtPath(actor_paths[0])
            camera_prim_path = None
            if actor_prim:
                for child in Usd.PrimRange(actor_prim):
                    if child.IsA(UsdGeom.Camera):
                        camera_prim_path = str(child.GetPath())
                        break

            if not camera_prim_path:
                # If no UsdGeomCamera child, use the actor path directly
                camera_prim_path = str(actor_paths[0])

            status = "capture enabled" if capture_enabled else "capture disabled"
            print(
                f"[CameraSensorManager] Found camera sensor: {sensor_name} "
                f"({width}x{height}, FOV={fov}, tick_rate={tick_rate}, {status}) "
                f"at {camera_prim_path}"
            )

            if not capture_enabled:
                continue

            # Validate parameters before setting up for capture
            if width <= 0 or height <= 0:
                carb.log_error(
                    f"[CameraSensorManager] Invalid resolution ({width}x{height}) for {sensor_name}, skipping"
                )
                continue
            if fov <= 0:
                carb.log_error(
                    f"[CameraSensorManager] Invalid FOV ({fov}) for {sensor_name}, skipping"
                )
                continue
            if tick_rate < 0:
                carb.log_error(
                    f"[CameraSensorManager] Invalid tick_rate ({tick_rate}) for {sensor_name}, skipping"
                )
                continue

            self._cameras[entity_id] = CameraSensorInfo(
                entity_id=entity_id,
                sensor_name=sensor_name,
                camera_prim_path=camera_prim_path,
                width=width,
                height=height,
                fov=fov,
                tick_rate=tick_rate,
            )

        if self._cameras:
            print(f"[CameraSensorManager] {len(self._cameras)} camera sensor(s) set up for capture")

    def initialize_cameras(self, world):
        """Create Isaac Sim Camera objects and add them to the World scene.

        Each Camera is created at the discovered prim path with the configured
        resolution and frequency. Adding to world.scene registers them so
        World.reset_async() will call camera.initialize() to set up render
        products and annotators.

        Args:
            world: The Isaac Sim World instance.
        """
        if self._cameras_initialized:
            return

        from isaacsim.sensors.camera import Camera

        for entity_id, cam_info in self._cameras.items():
            try:
                # Set aperture from configured FOV and resolution aspect ratio
                # before creating Camera (which validates aperture consistency)
                # horizontalAperture = 2 * focalLength * tan(hfov/2)
                # verticalAperture = horizontalAperture / aspectRatio
                if cam_info.fov > 0:
                    stage = world.stage
                    prim = stage.GetPrimAtPath(cam_info.camera_prim_path)
                    if prim and prim.IsValid():
                        geom_camera = UsdGeom.Camera(prim)
                        focal_length = geom_camera.GetFocalLengthAttr().Get()
                        hfov_rad = math.radians(cam_info.fov)
                        h_aperture = 2.0 * focal_length * math.tan(hfov_rad / 2.0)
                        aspect_ratio = cam_info.width / cam_info.height
                        v_aperture = h_aperture / aspect_ratio
                        geom_camera.GetHorizontalApertureAttr().Set(h_aperture)
                        geom_camera.GetVerticalApertureAttr().Set(v_aperture)

                # TODO: Apply tick_rate throttling based on AeroSim's simulation clock
                # For now, Camera captures every rendered frame
                camera = Camera(
                    prim_path=cam_info.camera_prim_path,
                    name=cam_info.sensor_name,
                    resolution=(cam_info.width, cam_info.height),
                )
                world.scene.add(camera)
                cam_info.camera = camera
                cam_info.initialized = True
                cam_info.frame_count = 0
                print(
                    f"[CameraSensorManager] Created Camera for: {cam_info.sensor_name} "
                    f"at {cam_info.camera_prim_path}"
                )
            except Exception as e:
                carb.log_error(
                    f"[CameraSensorManager] Failed to create camera {cam_info.sensor_name}: {e}"
                )

        self._cameras_initialized = True

    def capture_and_publish(self):
        """Capture RGBA frames from all initialized cameras and publish them.

        For each camera, calls Camera.get_rgba() to get a numpy array and
        publishes it via publish_image_to_topic() to the middleware.
        """
        if not self._cameras_initialized or not self._publish_fn:
            return

        for entity_id, cam_info in self._cameras.items():
            if not cam_info.initialized or cam_info.camera is None:
                continue

            # Wait a few frames after initialization for the renderer to warm up
            cam_info.frame_count += 1
            if cam_info.frame_count < 10:
                continue

            try:
                rgba = cam_info.camera.get_rgba()
                if rgba is None:
                    continue

                rgba = np.asarray(rgba, dtype=np.uint8)
                if rgba.size == 0:
                    continue

                # Publish RGBA8 image to middleware
                self._publish_fn(
                    RENDERER_RESPONSES_TOPIC,
                    cam_info.width,
                    cam_info.height,
                    IMAGE_FORMAT_RGBA8,
                    rgba,
                )
            except Exception as e:
                carb.log_error(
                    f"[CameraSensorManager] Capture failed for {cam_info.sensor_name}: {e}"
                )

    def get_cesium_viewports(self, stage):
        """Build Cesium Viewport objects for each initialized camera sensor.

        Computes view and projection matrices from each camera's UsdGeom.Camera
        prim so Cesium can include these frustums in tile selection.

        Returns:
            List of cesium.omniverse.bindings.Viewport, or empty list on error.
        """
        if not self._cameras_initialized:
            return []

        try:
            from cesium.omniverse.bindings import Viewport
        except ImportError:
            return []

        viewports = []
        for cam_info in self._cameras.values():
            if not cam_info.initialized:
                continue

            camera_prim = stage.GetPrimAtPath(cam_info.camera_prim_path)
            if not camera_prim or not camera_prim.IsValid():
                continue

            geom_camera = UsdGeom.Camera(camera_prim)
            if not geom_camera:
                continue

            gf_camera = geom_camera.GetCamera(Usd.TimeCode.Default())
            view_matrix = gf_camera.frustum.ComputeViewMatrix()
            proj_matrix = gf_camera.frustum.ComputeProjectionMatrix()

            viewport = Viewport()
            viewport.viewMatrix = view_matrix
            viewport.projMatrix = proj_matrix
            viewport.width = float(cam_info.width)
            viewport.height = float(cam_info.height)
            viewports.append(viewport)

        return viewports

    def cleanup(self):
        """Destroy Camera instances and release resources."""
        for entity_id, cam_info in self._cameras.items():
            try:
                if cam_info.camera is not None:
                    cam_info.camera.destroy()
                    cam_info.camera = None
            except Exception:
                pass
            cam_info.initialized = False
            cam_info.frame_count = 0

        self._cameras.clear()
        self._cameras_initialized = False
        print("[CameraSensorManager] Cleaned up all camera sensors")
