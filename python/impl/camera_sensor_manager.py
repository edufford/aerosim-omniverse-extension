"""
Camera sensor manager using Omniverse Replicator render products and annotators.

Discovers camera sensors from the AeroSim scene graph, creates render products
for each camera prim, captures RGBA frames via annotators, and publishes them
to the AeroSim middleware pipeline via publish_image_to_topic().
"""

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
        self.render_product = None
        self.annotator = None
        self.initialized = False
        self.frame_count = 0  # Track frames since initialization for warmup


class CameraSensorManager:
    """Manages camera render products for image capture and publishing."""

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

            # Skip sensors with capture_enabled = false (defaults to true if not set)
            capture_attr = sensor_prim.GetAttribute("sensor:parameters:capture_enabled")
            if capture_attr and capture_attr.Get() is False:
                continue

            # Read sensor parameters
            # Note: The C++ side may write zeros if it can't parse the Rust-serialized
            # sensor_parameters (enum wrapper + array resolution format mismatch).
            # Use sensible defaults when values are zero or missing.
            sensor_name = sensor_prim.GetAttribute("sensor:sensor_name").Get() or entity_id
            resolution = sensor_prim.GetAttribute("sensor:parameters:resolution").Get()
            tick_rate = sensor_prim.GetAttribute("sensor:parameters:tick_rate").Get() or 0.0
            fov = sensor_prim.GetAttribute("sensor:parameters:fov").Get() or 0.0

            width = resolution[0] if resolution and resolution[0] > 0 else 1920
            height = resolution[1] if resolution and resolution[1] > 0 else 1080
            if tick_rate <= 0:
                tick_rate = 0.02
            if fov <= 0:
                fov = 90.0

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

            self._cameras[entity_id] = CameraSensorInfo(
                entity_id=entity_id,
                sensor_name=sensor_name,
                camera_prim_path=camera_prim_path,
                width=width,
                height=height,
                fov=fov,
                tick_rate=tick_rate,
            )
            print(
                f"[CameraSensorManager] Discovered camera sensor: {sensor_name} "
                f"({width}x{height}, FOV={fov}, tick_rate={tick_rate}) "
                f"at {camera_prim_path}"
            )

        if self._cameras:
            print(f"[CameraSensorManager] Discovered {len(self._cameras)} camera sensor(s)")

    def initialize_cameras(self):
        """Create render products and RGBA annotators for each discovered sensor.

        Uses omni.replicator.core to create render products attached to each
        camera prim, then attaches an 'rgba' annotator to capture frames.
        This works in extension contexts without needing world.step().
        """
        if self._cameras_initialized:
            return

        try:
            import omni.replicator.core as rep
        except ImportError:
            carb.log_warn(
                "[CameraSensorManager] omni.replicator.core not available. "
                "Camera capture disabled."
            )
            return

        for entity_id, cam_info in self._cameras.items():
            try:
                # Create a render product for this camera at the desired resolution
                rp = rep.create.render_product(
                    cam_info.camera_prim_path,
                    (cam_info.width, cam_info.height),
                )

                # Create and attach an RGBA annotator
                annotator = rep.AnnotatorRegistry.get_annotator("LdrColor")
                annotator.attach([rp])

                cam_info.render_product = rp
                cam_info.annotator = annotator
                cam_info.initialized = True
                cam_info.frame_count = 0
                print(
                    f"[CameraSensorManager] Initialized render product for: {cam_info.sensor_name} "
                    f"at {cam_info.camera_prim_path}"
                )
            except Exception as e:
                carb.log_error(
                    f"[CameraSensorManager] Failed to initialize camera {cam_info.sensor_name}: {e}"
                )

        # Start the timeline so the OmniGraph pipeline feeds render products.
        # Without this, annotators return None because their data source is inactive.
        import omni.timeline
        timeline = omni.timeline.get_timeline_interface()
        timeline.play()
        print("[CameraSensorManager] Timeline started for render product capture")

        self._cameras_initialized = True

    def capture_and_publish(self):
        """Capture RGBA frames from all initialized cameras and publish them.

        For each camera, reads the annotator data to get a numpy array and
        publishes it via publish_image_to_topic() to the middleware.
        """
        if not self._cameras_initialized:
            print(f"[CameraSensorManager] capture_and_publish: not initialized (cameras_initialized={self._cameras_initialized}, cameras={len(self._cameras)})")
            return
        if not self._publish_fn:
            print("[CameraSensorManager] WARNING: publish function not set, skipping capture")
            return

        for entity_id, cam_info in self._cameras.items():
            if not cam_info.initialized or cam_info.annotator is None:
                continue

            # Wait a few frames after initialization for the renderer to warm up
            cam_info.frame_count += 1
            if cam_info.frame_count < 10:
                continue

            try:
                data = cam_info.annotator.get_data()
                if data is None:
                    if cam_info.frame_count % 100 == 0:
                        print(f"[CameraSensorManager] {cam_info.sensor_name}: annotator returned None (frame {cam_info.frame_count})")
                    continue

                # LdrColor annotator returns RGBA uint8 data
                rgba = np.array(data, dtype=np.uint8)
                if rgba.size == 0:
                    continue

                # Reshape if needed (annotator may return flat or shaped array)
                if rgba.ndim == 1:
                    rgba = rgba.reshape(cam_info.height, cam_info.width, 4)

                if cam_info.frame_count <= 15 or cam_info.frame_count % 100 == 0:
                    print(f"[CameraSensorManager] {cam_info.sensor_name}: captured {rgba.shape}, publishing to {RENDERER_RESPONSES_TOPIC}")

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
        """Destroy render products and release resources."""
        # Stop the timeline that was started for render product capture
        try:
            import omni.timeline
            timeline = omni.timeline.get_timeline_interface()
            timeline.stop()
        except Exception:
            pass

        for entity_id, cam_info in self._cameras.items():
            try:
                if cam_info.annotator is not None:
                    cam_info.annotator.detach()
                    cam_info.annotator = None
                cam_info.render_product = None
            except Exception:
                pass
            cam_info.initialized = False
            cam_info.frame_count = 0

        self._cameras.clear()
        self._cameras_initialized = False
        print("[CameraSensorManager] Cleaned up all camera sensors")
