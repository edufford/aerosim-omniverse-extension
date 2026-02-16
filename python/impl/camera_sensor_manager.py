"""
Camera sensor manager using Isaac Sim's isaacsim.sensors.camera API.

Discovers camera sensors from the AeroSim scene graph, creates Isaac Sim Camera
objects for each sensor, captures RGBA frames, and publishes them to the AeroSim
middleware pipeline via publish_image_to_topic().
"""

import carb

from pxr import Sdf, UsdGeom

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
        self.camera = None  # Isaac Sim Camera object
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
            entity_id = sensor_prim.GetPath().GetName()

            sensor_type_attr = sensor_prim.GetAttribute("sensor:sensor_type")
            if not sensor_type_attr:
                continue

            sensor_type = sensor_type_attr.Get()
            if sensor_type != "rgb_camera":
                continue

            # Read sensor parameters
            sensor_name = sensor_prim.GetAttribute("sensor:sensor_name").Get() or entity_id
            resolution = sensor_prim.GetAttribute("sensor:parameters:resolution").Get()
            tick_rate = sensor_prim.GetAttribute("sensor:parameters:tick_rate").Get() or 0.1
            fov = sensor_prim.GetAttribute("sensor:parameters:fov").Get() or 90.0

            width = resolution[0] if resolution else 1920
            height = resolution[1] if resolution else 1080

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
                for child in actor_prim.GetDescendants():
                    if child.IsA(UsdGeom.Camera):
                        camera_prim_path = str(child.GetPath())
                        break

            if not camera_prim_path:
                # If no UsdGeomCamera child, use the actor path directly
                camera_prim_path = str(actor_paths[0])

            self._cameras[entity_id] = CameraSensorInfo(
                entity_id=entity_id,
                sensor_name=sensor_name,
                camera_prim_path=camera_prim_path,
                width=width,
                height=height,
                fov=fov,
                tick_rate=tick_rate,
            )
            carb.log_info(
                f"[CameraSensorManager] Discovered camera sensor: {sensor_name} "
                f"({width}x{height}, FOV={fov}, tick_rate={tick_rate}) "
                f"at {camera_prim_path}"
            )

        if self._cameras:
            carb.log_info(f"[CameraSensorManager] Discovered {len(self._cameras)} camera sensor(s)")

    def initialize_cameras(self):
        """Create Isaac Sim Camera objects for each discovered sensor.

        Uses isaacsim.sensors.camera.Camera to create render products
        attached to the camera prim paths discovered from the scene graph.
        """
        if self._cameras_initialized:
            return

        try:
            from isaacsim.sensors.camera import Camera
        except ImportError:
            carb.log_warn(
                "[CameraSensorManager] isaacsim.sensors.camera not available. "
                "Camera capture disabled."
            )
            return

        for entity_id, cam_info in self._cameras.items():
            try:
                camera = Camera(
                    prim_path=cam_info.camera_prim_path,
                    resolution=(cam_info.width, cam_info.height),
                )
                camera.initialize()
                camera.add_motion_vectors_to_frame()

                cam_info.camera = camera
                cam_info.frame_count = 0
                carb.log_info(
                    f"[CameraSensorManager] Initialized camera: {cam_info.sensor_name} "
                    f"at {cam_info.camera_prim_path}"
                )
            except Exception as e:
                carb.log_error(
                    f"[CameraSensorManager] Failed to initialize camera {cam_info.sensor_name}: {e}"
                )

        self._cameras_initialized = True

    def capture_and_publish(self):
        """Capture RGBA frames from all initialized cameras and publish them.

        For each camera, calls get_rgba() to get a numpy array of shape
        (height, width, 4) and publishes it via publish_image_to_topic()
        to the 'aerosim.renderer.responses' topic.
        """
        if not self._cameras_initialized or not self._publish_fn:
            return

        for entity_id, cam_info in self._cameras.items():
            if cam_info.camera is None:
                continue

            # Wait a few frames after initialization for the renderer to warm up
            cam_info.frame_count += 1
            if cam_info.frame_count < 5:
                continue

            try:
                rgba = cam_info.camera.get_rgba()
                if rgba is None or rgba.size == 0:
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

    def cleanup(self):
        """Destroy Camera objects and release resources."""
        for entity_id, cam_info in self._cameras.items():
            if cam_info.camera is not None:
                try:
                    cam_info.camera = None
                except Exception:
                    pass
            cam_info.initialized = False
            cam_info.frame_count = 0

        self._cameras.clear()
        self._cameras_initialized = False
        carb.log_info("[CameraSensorManager] Cleaned up all camera sensors")
