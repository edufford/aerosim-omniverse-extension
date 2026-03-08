// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// NVIDIA CORPORATION and its licensors retain all intellectual property
// and proprietary rights in and to this software, related documentation
// and any modifications thereto.  Any use, reproduction, disclosure or
// distribution of this software and related documentation without an express
// license agreement from NVIDIA CORPORATION is strictly prohibited.
//

#define CARB_EXPORTS
#define _CRT_SECURE_NO_WARNINGS  // allow using std::getenv() for Win/Linux

#include <carb/PluginUtils.h>

#include <aerosim/omniverse/extension/IAerosimConnector.h>
#include <omni/ext/ExtensionsUtils.h>
#include <omni/ext/IExt.h>
#include <omni/kit/IApp.h>

#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/camera.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
// #include <pxr/base/gf/matrix4f.h>
// #include <pxr/base/gf/matrix4d.h>
// #include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/tf/token.h>

#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdGeom/xform.h>

extern "C" {
  #include "aerosim_world_link.h"
}

#include <aerosim/omniverse/extension/SceneGraphComponents.h>
#include <aerosim/omniverse/extension/SceneGraphResources.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace PXR_NS;

#include <vector>
#include <unordered_map>
#include <fstream>
#include <cstdlib>
#include <cmath>

const struct carb::PluginImplDesc pluginImplDesc = { "aerosim.omniverse.extension.plugin",
                                                     "Aerosim Omniverse Connection Plugin.", "AeroSim",
                                                     carb::PluginHotReload::eEnabled, "dev" };


namespace aerosim
{
namespace connector
{

class AerosimConnector : public IAerosimConnector, public PXR_NS::TfWeakBase
{
protected:

    std::string getAssetRootPath() const
    {
        const char* assets_root_path = std::getenv("AEROSIM_ASSETS_ROOT");
        if (assets_root_path != nullptr) {
            std::string assets_root_path_str(assets_root_path);
            printf("AEROSIM_ASSETS_ROOT environment variable is: %s.\n", assets_root_path_str.c_str());
            return assets_root_path_str;
        }
        else
        {
            printf("Failed to get the AEROSIM_ASSETS_ROOT environment variable.\n");
            return "";
        }
    }

    void processSceneGraphJson(const json& data) {
        if (data.contains("resources")) {
            for (auto it = data["resources"].begin(); it != data["resources"].end(); ++it) {
                std::string resource_name = it.key();
                const json& resource_data = it.value();

                // Construct the prim path.
                PXR_NS::SdfPath primPath("/Resources");
                primPath = primPath.AppendChild(TfToken(resource_name));

                // Retrieve or define the prim.
                PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(primPath);
                if (!prim.IsValid()) {
                    prim = m_stage->DefinePrim(primPath);

                    // Pre-create viewport_config attributes so Fabric doesn't
                    // warn about missing properties before the scene graph is initialized.
                    if (resource_name == "viewport_config") {
                        prim.CreateAttribute(TfToken("disable_viewport_config"), SdfValueTypeNames->Bool);
                        prim.CreateAttribute(TfToken("active_camera"), SdfValueTypeNames->String);
                        prim.CreateAttribute(TfToken("active_camera_path"), SdfValueTypeNames->String);
                    }
                }

                if (resource_name == "origin") {
                    Origin origin = resource_data.get<Origin>();
                    origin.resource_id = "origin";
                    origin.resource_type = "coordinates";

                    UsdAttribute latitudeAttr = prim.CreateAttribute(TfToken("latitude"), SdfValueTypeNames->Double);
                    latitudeAttr.Set(origin.latitude);

                    UsdAttribute longitudeAttr = prim.CreateAttribute(TfToken("longitude"), SdfValueTypeNames->Double);
                    longitudeAttr.Set(origin.longitude);

                    UsdAttribute altitudeAttr = prim.CreateAttribute(TfToken("altitude"), SdfValueTypeNames->Double);
                    altitudeAttr.Set(origin.altitude);
                }
                 else if (resource_name == "sim_time") {
                    TimeStamp sim_time = resource_data.get<TimeStamp>();
                    sim_time.resource_id = "sim_time";
                    sim_time.resource_type = "time";

                    UsdAttribute secAttr = prim.CreateAttribute(TfToken("sec"), SdfValueTypeNames->Double);
                    secAttr.Set(sim_time.sec);

                    UsdAttribute nsecAttr = prim.CreateAttribute(TfToken("nsec"), SdfValueTypeNames->Double);
                    nsecAttr.Set(sim_time.nsec);
                }
                else if (resource_name == "viewport_config" && m_sceneGraphInitialized) {
                    ViewportConfig viewport_config = resource_data.get<ViewportConfig>();
                    viewport_config.resource_id = "viewport_config";
                    viewport_config.resource_type = "viewport_config";

                    UsdAttribute enabledAttr = prim.CreateAttribute(TfToken("disable_viewport_config"), SdfValueTypeNames->Bool);
                    UsdAttribute activeCameraAttr = prim.CreateAttribute(TfToken("active_camera"), SdfValueTypeNames->String);
                    UsdAttribute activeCameraPathAttr = prim.CreateAttribute(TfToken("active_camera_path"), SdfValueTypeNames->String);

                    activeCameraAttr.Set(viewport_config.active_camera);
                    if (viewport_config.active_camera == "None" || viewport_config.active_camera == "") {
                        activeCameraPathAttr.Set(std::string(""));
                        break;
                    }

                    // Get the entity prim
                    PXR_NS::SdfPath cameraEntityPath = SdfPath("/Entities").AppendChild(TfToken(viewport_config.active_camera));
                    if (!m_stage->GetPrimAtPath(cameraEntityPath)) {
                        printf("WARNING: Couldn't get viewport_config.active_camera entity '%s'.\n", cameraEntityPath.GetText());
                        activeCameraAttr.Set("None");
                        activeCameraPathAttr.Set(std::string(""));
                        break;
                    }

                    //Get the camera actor prim from entity prim
                    UsdRelationship actorRel = m_stage->GetPrimAtPath(cameraEntityPath).GetRelationship(TfToken("actor_ref"));
                    if (!actorRel) {
                        printf("WARNING: Couldn't get actor relationship for viewport_config.active_camera entity '%s'.\n", cameraEntityPath.GetText());
                        activeCameraAttr.Set("None");
                        activeCameraPathAttr.Set(std::string(""));
                        break;
                    }

                    PXR_NS::SdfPathVector actor_references;
                    actorRel.GetForwardedTargets(&actor_references);
                    if (actor_references.size() == 0) {
                        printf("WARNING: Couldn't get actor references for viewport_config.active_camera entity '%s'.\n", cameraEntityPath.GetText());
                        activeCameraAttr.Set("None");
                        activeCameraPathAttr.Set(std::string(""));
                        break;
                    }

                    PXR_NS::SdfPath actorPath = actor_references[0];
                    PXR_NS::UsdPrim actorPrim = m_stage->GetPrimAtPath(actorPath);
                    if (!actorPrim) {
                        printf("WARNING: Couldn't get prim for viewport_config.active_camera entity '%s'.\n", cameraEntityPath.GetText());
                        activeCameraAttr.Set("None");
                        activeCameraPathAttr.Set(std::string(""));
                        break;
                    }

                    // Iterate over the actor prim's children to find a valid camera prim.
                    PXR_NS::UsdPrim cameraPrim;
                    for (const UsdPrim& child: UsdPrimRange(actorPrim)) {
                        if (child.IsA<UsdGeomCamera>()) {
                            cameraPrim = child;
                            break;
                        }
                    }

                    if (!cameraPrim) {
                        printf("WARNING: Couldn't find a valid camera prim for viewport_config.active_camera entity '%s'.\n", cameraEntityPath.GetText());
                        activeCameraAttr.Set("None");
                        activeCameraPathAttr.Set(std::string(""));
                        break;
                    }

                    // Set the found camera prim path as the active camera.
                    activeCameraPathAttr.Set(cameraPrim.GetPath().GetString());
                }
            }
        }

        if (data.contains("components")) {
            for (auto it = data["components"].begin(); it != data["components"].end(); ++it) {
                std::string component_name = it.key();
                const json& component_data = it.value();

                for (auto entity_it = component_data.begin(); entity_it != component_data.end(); ++entity_it) {
                    std::string entity_id = entity_it.key();
                    const json& entity_data = entity_it.value();

                    // Construct the prim path: /Components/<component_name>/<entity_id>
                    PXR_NS::SdfPath primPath("/Components");
                    primPath = primPath.AppendChild(TfToken(component_name))
                                    .AppendChild(TfToken(entity_id));

                    // Retrieve or define the prim.
                    PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(primPath);
                    if (!prim.IsValid()) {
                        prim = m_stage->DefinePrim(primPath);
                    }

                    if (component_name == "actor_properties") {
                        ActorProperties ap = entity_data.get<ActorProperties>();
                        ap.owner_entity_id = entity_id;
                        ap.component_type = component_name;

                        UsdAttribute actorNameAttr = prim.CreateAttribute(TfToken("actor_name"), SdfValueTypeNames->String);
                        actorNameAttr.Set(ap.actor_name);

                        UsdAttribute actorAssetAttr = prim.CreateAttribute(TfToken("actor_asset"), SdfValueTypeNames->String);
                        actorAssetAttr.Set(ap.actor_asset);

                        UsdAttribute parentAttr = prim.CreateAttribute(TfToken("parent"), SdfValueTypeNames->String);
                        parentAttr.Set(ap.parent);

                    } else if (component_name == "actor_state") {
                        ActorState actorState = entity_data.get<ActorState>();
                        actorState.owner_entity_id = entity_id;
                        actorState.component_type = component_name;
                        // TODO This assumes every entity with an actor_state component also has an actor_properties component.
                        // Change this to use a `is_relative_pose` bool in the actor_state component instead of checking the parent string.
                        // auto comp_data = data["components"]["actor_properties"][entity_id];
                        auto actor_props = data["components"]["actor_properties"][entity_id];
                        std::string parent = actor_props["parent"];

                        // Create custom attributes for transform data
                        auto posAttr = prim.CreateAttribute(TfToken("pose:position"), SdfValueTypeNames->Double3);
                        double pos_x = actorState.pose.transform.position.x;
                        double pos_y = actorState.pose.transform.position.y;
                        double pos_z = actorState.pose.transform.position.z;
                        if (parent.size() == 0) {
                            ned_to_enu(&pos_x, &pos_y, &pos_z);
                        } else {
                            frd_to_flu(&pos_x, &pos_y, &pos_z);
                        }
                        posAttr.Set(GfVec3d(pos_x, pos_y, pos_z));

                        auto orientAttr = prim.CreateAttribute(TfToken("pose:orientation"), SdfValueTypeNames->Quatd);
                        double roll;
                        double pitch;
                        double yaw;

                        // Input quaternion from scene graph (NED/FRD frame)
                        double qw = actorState.pose.transform.orientation.w;
                        double qx = actorState.pose.transform.orientation.x;
                        double qy = actorState.pose.transform.orientation.y;
                        double qz = actorState.pose.transform.orientation.z;

                        // Decompose to RPY using Intrinsic ZYX
                        aerosim_quat_wxyz_to_rpy(qw, qx, qy, qz, &roll, &pitch, &yaw);

                        // Convert FRD body frame to FLU body frame
                        rpy_frd_to_flu(&roll, &pitch, &yaw);

                        if (parent.size() == 0) {
                            // For global poses, rotate FLU (NWU) to Cesium's ENU
                            rpy_nwu_to_enu(&roll, &pitch, &yaw);
                        }

                        // Convert RPY to quaternion using aerospace Intrinsic ZYX
                        // (Tait-Bryan) convention: yaw about Z first, then pitch about
                        // new Y, then roll about newest X.
                        // USD uses row-vector convention where r1 * r2 applies r1 first
                        // (left-to-right). Intrinsic ZYX = Extrinsic XYZ, so composition
                        // order is X * Y * Z.
                        auto rotation_x = GfRotation(GfVec3d::XAxis(), to_degrees(roll));
                        auto rotation_y = GfRotation(GfVec3d::YAxis(), to_degrees(pitch));
                        auto rotation_z = GfRotation(GfVec3d::ZAxis(), to_degrees(yaw));
                        auto rotation = rotation_x * rotation_y * rotation_z;
                        auto rotation_quat = rotation.GetQuat();
                        orientAttr.Set(rotation_quat);

                        auto scaleAttr = prim.CreateAttribute(TfToken("pose:scale"), SdfValueTypeNames->Double3);
                        scaleAttr.Set(GfVec3d(
                            actorState.pose.transform.scale.x,
                            actorState.pose.transform.scale.y,
                            actorState.pose.transform.scale.z));

                        // NED attribute (Double3): [north, east, down]
                        auto nedAttr = prim.CreateAttribute(TfToken("world_coordinate:ned"), SdfValueTypeNames->Double3);
                        nedAttr.Set(PXR_NS::GfVec3d(
                            actorState.worldCoordinate.ned.north,
                            actorState.worldCoordinate.ned.east,
                            actorState.worldCoordinate.ned.down));

                        // LLA attribute (Double3): [latitude, longitude, altitude]
                        auto llaAttr = prim.CreateAttribute(TfToken("world_coordinate:lla"), SdfValueTypeNames->Double3);
                        llaAttr.Set(PXR_NS::GfVec3d(
                            actorState.worldCoordinate.lla.latitude,
                            actorState.worldCoordinate.lla.longitude,
                            actorState.worldCoordinate.lla.altitude));

                        // ECEF attribute (Double3): assuming actorState.world_coordinate.ecef is compatible with GfVec3d
                        auto ecefAttr = prim.CreateAttribute(TfToken("world_coordinate:ecef"), SdfValueTypeNames->Double3);
                        ecefAttr.Set(PXR_NS::GfVec3d(
                            actorState.worldCoordinate.ecef.x,
                            actorState.worldCoordinate.ecef.y,
                            actorState.worldCoordinate.ecef.z));

                        // Cartesian attribute (Double3)
                        auto cartesianAttr = prim.CreateAttribute(TfToken("world_coordinate:cartesian"), SdfValueTypeNames->Double3);
                        cartesianAttr.Set(PXR_NS::GfVec3d(
                            actorState.worldCoordinate.cartesian.x,
                            actorState.worldCoordinate.cartesian.y,
                            actorState.worldCoordinate.cartesian.z));

                        // Origin LLA attribute (Double3)
                        auto originLLAAttr = prim.CreateAttribute(TfToken("world_coordinate:origin_lla"), SdfValueTypeNames->Double3);
                        originLLAAttr.Set(PXR_NS::GfVec3d(
                            actorState.worldCoordinate.origin_lla.latitude,
                            actorState.worldCoordinate.origin_lla.longitude,
                            actorState.worldCoordinate.origin_lla.altitude));

                        // Ellipsoid attribute (Double3): [equatorial_radius, flattening_factor, polar_radius]
                        auto ellipsoidAttr = prim.CreateAttribute(TfToken("world_coordinate:ellipsoid"), SdfValueTypeNames->Double3);
                        ellipsoidAttr.Set(PXR_NS::GfVec3d(
                            actorState.worldCoordinate.ellipsoid.equatorial_radius,
                            actorState.worldCoordinate.ellipsoid.flattening_factor,
                            actorState.worldCoordinate.ellipsoid.polar_radius));

                    } else if (component_name == "sensor") {
                        Sensor sensor = entity_data.get<Sensor>();
                        sensor.owner_entity_id = entity_id;
                        sensor.component_type = component_name;

                        // Sensor name attribute.
                        auto sensorNameAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:sensor_name"), SdfValueTypeNames->String);
                        sensorNameAttr.Set(sensor.sensor_name);

                        // Sensor type attribute.
                        auto sensorTypeAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:sensor_type"), SdfValueTypeNames->String);
                        sensorTypeAttr.Set(sensor.sensor_type);

                        // Sensor parameters:
                        // Resolution (Vector2i) stored as an Int2.
                        auto resolutionAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:parameters:resolution"), SdfValueTypeNames->Int2);
                        // Convert Vector2i to GfVec2i. (Assuming sensor.sensor_parameters.resolution has x and y members.)
                        resolutionAttr.Set(PXR_NS::GfVec2i(sensor.sensor_parameters.resolution.x, sensor.sensor_parameters.resolution.y));

                        // Tick rate attribute (Double).
                        auto tickRateAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:parameters:tick_rate"), SdfValueTypeNames->Double);
                        tickRateAttr.Set(sensor.sensor_parameters.tick_rate);

                        // Field of view attribute (Double).
                        auto fovAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:parameters:fov"), SdfValueTypeNames->Double);
                        fovAttr.Set(sensor.sensor_parameters.fov);

                        // Near clip attribute (Double).
                        auto nearClipAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:parameters:near_clip"), SdfValueTypeNames->Double);
                        nearClipAttr.Set(sensor.sensor_parameters.near_clip);

                        // Far clip attribute (Double).
                        auto farClipAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:parameters:far_clip"), SdfValueTypeNames->Double);
                        farClipAttr.Set(sensor.sensor_parameters.far_clip);

                        // Capture enabled attribute (Bool).
                        auto captureEnabledAttr = prim.CreateAttribute(PXR_NS::TfToken("sensor:parameters:capture_enabled"), SdfValueTypeNames->Bool);
                        captureEnabledAttr.Set(sensor.sensor_parameters.capture_enabled);

                    } else if (component_name == "effectors") {
                        Effectors effectors = entity_data.get<Effectors>();
                        effectors.owner_entity_id = entity_id;
                        effectors.component_type = component_name;

                        for (auto it = effectors.effectors.begin(); it != effectors.effectors.end(); ++it) {
                            Effector effector = *it;
                            PXR_NS::SdfPath effectorPath = primPath.AppendChild(TfToken(effector.effector_id));

                            PXR_NS::UsdPrim effectorPrim = m_stage->GetPrimAtPath(effectorPath);
                            if (!effectorPrim.IsValid()) {
                                effectorPrim = m_stage->DefinePrim(effectorPath);
                            }

                            UsdAttribute effectorNameAttr = effectorPrim.CreateAttribute(TfToken("effector_id"), SdfValueTypeNames->String);
                            effectorNameAttr.Set(effector.effector_id);

                            UsdAttribute effectorPathAttr = effectorPrim.CreateAttribute(TfToken("relative_path"), SdfValueTypeNames->String);
                            effectorPathAttr.Set(effector.relative_path);

                            UsdAttribute pose_position = effectorPrim.CreateAttribute(TfToken("pose:position"), SdfValueTypeNames->Double3);
                            UsdAttribute pose_orientation = effectorPrim.CreateAttribute(TfToken("pose:rotationXYZ"), SdfValueTypeNames->Float3);
                            UsdAttribute pose_scale = effectorPrim.CreateAttribute(TfToken("pose:scale"), SdfValueTypeNames->Float3);

                            // --- Position ---
                            UsdAttribute initial_position = effectorPrim.GetAttribute(TfToken("initial_pose:position"));
                            if (initial_position) {
                                GfVec3d initPos;
                                if (initial_position.Get(&initPos)) {
                                    GfVec3d inputPos(
                                        effector.pose.transform.position.x,
                                        effector.pose.transform.position.y,
                                        effector.pose.transform.position.z
                                    );
                                    GfVec3d resultPos = initPos + inputPos;
                                    pose_position.Set(resultPos);
                                }
                            }

                            // --- Rotation ---
                            UsdAttribute initial_orientation = effectorPrim.GetAttribute(TfToken("initial_pose:rotationXYZ"));
                            if (initial_orientation) {
                                GfVec3f initEuler;
                                if (initial_orientation.Get(&initEuler)) {
                                    double roll;
                                    double pitch;
                                    double yaw;
                                    aerosim_quat_wxyz_to_rpy(effector.pose.transform.orientation.w,
                                                            effector.pose.transform.orientation.x,
                                                            effector.pose.transform.orientation.y,
                                                            effector.pose.transform.orientation.z,
                                                            &roll, &pitch, &yaw);
                                    rpy_frd_to_flu(&roll, &pitch, &yaw);

                                    // Convert from radians to degrees
                                    roll = roll * 180.0 / M_PI;
                                    pitch = pitch * 180.0 / M_PI;
                                    yaw = yaw * 180.0 / M_PI;

                                    // Combine the initial Euler angles with the effector's Euler angles
                                    GfVec3f resultEuler = initEuler + GfVec3f(roll, pitch, yaw);
                                    pose_orientation.Set(resultEuler);
                                }
                            }

                            // --- Scale ---
                            UsdAttribute initial_scale = effectorPrim.GetAttribute(TfToken("initial_pose:scale"));
                            if (initial_scale) {
                                GfVec3f initScale;
                                if (initial_scale.Get(&initScale)) {
                                    GfVec3f scalePose(
                                        initScale[0] * effector.pose.transform.scale.x,
                                        initScale[1] * effector.pose.transform.scale.y,
                                        initScale[2] * effector.pose.transform.scale.z
                                    );
                                    pose_scale.Set(scalePose);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (data.contains("entities")) {
            for (auto entity_it = data["entities"].begin(); entity_it != data["entities"].end(); ++entity_it) {
                std::string entity_id = entity_it.key();
                const json& component_list = entity_it.value();

                // Construct the prim path: /Components/<component_name>/<entity_id>
                PXR_NS::SdfPath primPath("/Entities");
                primPath = primPath.AppendChild(TfToken(entity_id));

                // Retrieve or define the prim.
                PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(primPath);
                if (!prim.IsValid()) {
                    prim = m_stage->DefinePrim(primPath);
                }

                UsdAttribute componentsAttr = prim.CreateAttribute(TfToken("components"), SdfValueTypeNames->StringArray);

                VtArray<std::string> componentPaths;
                for (const auto& comp : component_list) {
                    std::string component_name = comp.get<std::string>();
                    PXR_NS::SdfPath componentPrimPath("/Components");

                    componentPrimPath = componentPrimPath.AppendChild(TfToken(component_name)).AppendChild(TfToken(entity_id));
                    componentPaths.push_back(componentPrimPath.GetString());
                }
                componentsAttr.Set(componentPaths);
            }
        }
    }

    void constructActorHierarchy() {
        PXR_NS::SdfPath actorPropertiesPath("/Components/actor_properties");
        PXR_NS::UsdPrim actorPropertiesPrim = m_stage->GetPrimAtPath(actorPropertiesPath);
        if (!actorPropertiesPrim) {
            printf("Failed to get actor properties prim.\n");
            return;
        }

        for (const auto& actorPropertiesPrim : actorPropertiesPrim.GetChildren()) {
            // Create actor prim
            PXR_NS::SdfPath primPath("/World");
            std::deque<std::string> pathSegments;
            std::unordered_set<std::string> visited;

            // Start from the current actor; keep moving up parents until empty.
            std::string current_entity_parent;
            if (!actorPropertiesPrim.GetAttribute(TfToken("parent")).Get(&current_entity_parent)) {
                printf("Failed to get parent attribute.\n");
            }
            std::string current_entity_actor_name;
            if (!actorPropertiesPrim.GetAttribute(TfToken("actor_name")).Get(&current_entity_actor_name)) {
                printf("Failed to get actor_name attribute.\n");
            }

            pathSegments.push_front(current_entity_actor_name);

            while (current_entity_parent != "") {
                SdfPath parent_actor_properties_component_path = SdfPath("/Components/actor_properties/" + current_entity_parent);
                PXR_NS::UsdPrim parent_actor_properties_prim = m_stage->GetPrimAtPath(parent_actor_properties_component_path);
                if (!parent_actor_properties_prim) {
                    printf("Failed to get parent actor properties prim.\n");
                    break;
                }

                // Move one level up in the hierarchy.
                if (!parent_actor_properties_prim.GetAttribute(TfToken("parent")).Get(&current_entity_parent)) {
                    printf("Failed to get parent attribute.\n");
                    break;
                }
                if (!parent_actor_properties_prim.GetAttribute(TfToken("actor_name")).Get(&current_entity_actor_name)) {
                    printf("Failed to get actor_name attribute.\n");
                    break;
                }

                // Push this actor's name at the front.
                pathSegments.push_front(current_entity_actor_name);
            }

            for (const std::string& segment : pathSegments) {
                primPath = primPath.AppendChild(TfToken(segment));
            }

            if (m_stage->GetPrimAtPath(primPath))
            {
                // A prim already exists at this path.
                printf("A prim already exists at path: %s\n", primPath.GetText());
                // return;
            }

            PXR_NS::UsdGeomXform xformPrim = PXR_NS::UsdGeomXform::Define(m_stage, primPath);
            //Create xform ops for translation, orientation, and scale
            auto translateOp = xformPrim.AddTranslateOp(UsdGeomXformOp::PrecisionDouble);
            auto orientationOp = xformPrim.AddOrientOp(UsdGeomXformOp::PrecisionDouble);
            auto scaleOp = xformPrim.AddScaleOp(UsdGeomXformOp::PrecisionDouble);

            std::string actorAsset;
            if (!actorPropertiesPrim.GetAttribute(TfToken("actor_asset")).Get(&actorAsset)) {
                printf("Failed to get parent attribute.\n");
            }

            std::string assetPath = getAssetRootPath() + "/" + actorAsset + ".usdc";
            xformPrim.GetPrim().GetReferences().AddReference(assetPath);

            auto entity_id = actorPropertiesPrim.GetPath().GetName();
            PXR_NS::SdfPath entityPath("/Entities");
            entityPath = entityPath.AppendChild(TfToken(entity_id));

            auto prim = m_stage->GetPrimAtPath(entityPath);
            if (!prim) {
                printf("Failed to get entity prim.\n");
                return;
            }
            UsdRelationship actorRel = prim.CreateRelationship(TfToken("actor_ref"));
            actorRel.AddTarget(primPath);
        }

        // Set up initial pose data for effector components
        PXR_NS::SdfPath effectorsComponentPath("/Components/effectors");
        PXR_NS::UsdPrim effectorsComponentPrim = m_stage->GetPrimAtPath(effectorsComponentPath);
        if (!effectorsComponentPrim) {
            printf("Failed to get effectors component prim.\n");
            return;
        }

        for (const auto& effectorsPrim : effectorsComponentPrim.GetChildren()) {
            auto entity_id = effectorsPrim.GetPath().GetName();
            PXR_NS::SdfPath entityPath("/Entities");
            entityPath = entityPath.AppendChild(TfToken(entity_id));

            auto entityPrim = m_stage->GetPrimAtPath(entityPath);
            if (!entityPrim) {
                printf("Failed to get entity prim.\n");
                return;
            }

            // Get actor prim
            UsdRelationship actorRel = entityPrim.GetRelationship(TfToken("actor_ref"));
            SdfPathVector actorPaths;

            actorRel.GetForwardedTargets(&actorPaths);
            if (actorPaths.size() == 0) {
                printf("Failed to get actor path.\n");
                continue;
            }

            SdfPath baseActorPath = actorPaths[0];
            for (const auto& effectorPrim : effectorsPrim.GetChildren()) {

                // Get effector relative path
                std::string relative_path;
                if (!effectorPrim.GetAttribute(TfToken("relative_path")).Get(&relative_path)) {
                    printf("Failed to get relative path attribute.\n");
                    continue;
                }

                // Get effector actor prim from base actor prim and relative path
                std::string combinedPath = std::string(baseActorPath.GetText()) + "/" + relative_path;
                SdfPath effectorActorPath(combinedPath);

                auto effectorActorPrim = m_stage->GetPrimAtPath(effectorActorPath);
                if (!effectorActorPrim) {
                    printf("Failed to get effector actor prim at path: %s\n", combinedPath.c_str());
                    continue;
                }

                // create effector_ref relationship
                UsdRelationship effectorRel = effectorPrim.CreateRelationship(TfToken("effector_ref"));
                effectorRel.AddTarget(effectorActorPath);

                // Use effectorPrim instead of actorPrim.
                PXR_NS::UsdGeomXformable effectorXformable(effectorActorPrim);
                if (!effectorXformable) {
                    printf("Failed to get effector xformable.\n");
                    return;
                }

                bool resetXformStack = true;
                auto effectorsXformOps = effectorXformable.GetOrderedXformOps(&resetXformStack);
                if (effectorsXformOps.empty()) {
                    printf("Failed to get effector xform ops.\n");
                    return;
                }

                for (auto& xformOp : effectorsXformOps) {
                    if (xformOp.GetOpType() == UsdGeomXformOp::TypeTranslate) {
                        // Get the translation op attribute.
                        UsdAttribute posXformAttr = xformOp.GetAttr();
                        GfVec3d trans;
                        if (!posXformAttr.Get(&trans)) {
                            printf("Failed to get translation value.\n");
                        } else {
                            // Create and set the initial_pose:position attribute.
                            UsdAttribute initial_position = effectorPrim.CreateAttribute(
                                TfToken("initial_pose:position"),
                                SdfValueTypeNames->Double3
                            );
                            initial_position.Set(trans);
                        }
                    }
                    else if (xformOp.GetOpType() == UsdGeomXformOp::TypeRotateXYZ) {
                        // Get the orientation op attribute.
                        UsdAttribute orientXformAttr = xformOp.GetAttr();
                        if (orientXformAttr) {
                            GfVec3f rot;
                            if (orientXformAttr.Get(&rot)) {
                                // Create and set the initial_pose:rotationXYZ attribute.
                                UsdAttribute initial_orientation = effectorPrim.CreateAttribute(
                                    TfToken("initial_pose:rotationXYZ"),
                                    SdfValueTypeNames->Float3
                                );

                                initial_orientation.Set(rot);
                            }
                        }
                    }
                    else if (xformOp.GetOpType() == UsdGeomXformOp::TypeScale) {
                        // Get the scale op attribute.
                        UsdAttribute scaleXformAttr = xformOp.GetAttr();
                        GfVec3f scale;
                        if (!scaleXformAttr.Get(&scale)) {
                            printf("Failed to get scale value.\n");
                        } else {
                            // Create and set the initial_pose:scale attribute.
                            UsdAttribute initial_scale = effectorPrim.CreateAttribute(
                                TfToken("initial_pose:scale"),
                                SdfValueTypeNames->Float3
                            );
                            initial_scale.Set(GfVec3f(scale[0], scale[1], scale[2]));
                        }
                    }
                }
            }
        }
    }

    void configureAttributeConnections() {
        //Setup Cesium Origin connection
        PXR_NS::SdfPath originPath("/Resources/origin");
        PXR_NS::UsdPrim originPrim = m_stage->GetPrimAtPath(originPath);
        if (!originPrim) {
            printf("Failed to get origin prim.\n");
            return;
        }
        PXR_NS::SdfPath cesiumGeoreferencePath("/CesiumGeoreference");
        PXR_NS::UsdPrim cesiumGeoreferencePrim = m_stage->GetPrimAtPath(cesiumGeoreferencePath);
        if (!cesiumGeoreferencePrim) {
            printf("Failed to get cesium georeference prim.\n");
            return;
        }
        auto origin_latitude_path = originPrim.GetAttribute(TfToken("latitude")).GetPath();
        cesiumGeoreferencePrim.GetAttribute(TfToken("cesium:georeferenceOrigin:latitude")).AddConnection(origin_latitude_path);

        auto origin_longitude_path = originPrim.GetAttribute(TfToken("longitude")).GetPath();
        cesiumGeoreferencePrim.GetAttribute(TfToken("cesium:georeferenceOrigin:longitude")).AddConnection(origin_longitude_path);

        auto origin_altitude_path = originPrim.GetAttribute(TfToken("altitude")).GetPath();
        cesiumGeoreferencePrim.GetAttribute(TfToken("cesium:georeferenceOrigin:height")).AddConnection(origin_altitude_path);

        //Set up Transform connections
        PXR_NS::SdfPath entityMapPath("/Entities");
        PXR_NS::UsdPrim entityMapPrim = m_stage->GetPrimAtPath(entityMapPath);
        if (!entityMapPrim) {
            printf("Failed to get entity map prim.\n");
            return;
        }

        for (const auto& entityPrim : entityMapPrim.GetChildren()) {
            PXR_NS::SdfPath entityPath = entityPrim.GetPath();
            UsdRelationship actorRel = entityPrim.GetRelationship(TfToken("actor_ref"));
            SdfPathVector actorPaths;

            actorRel.GetForwardedTargets(&actorPaths);
            if (actorPaths.size() == 0) {
                printf("Failed to get actor path.\n");
                continue;
            }

            SdfPath actorPath = actorPaths[0];
            PXR_NS::UsdPrim actorPrim = m_stage->GetPrimAtPath(actorPath);
            if (!actorPrim) {
                printf("Failed to get actor prim.\n");
                continue;
            }

            // Get the actor's pose data
            PXR_NS::SdfPath actorStatePath("/Components/actor_state");
            actorStatePath = actorStatePath.AppendChild(TfToken(entityPrim.GetPath().GetName()));
            PXR_NS::UsdPrim actorStatePrim = m_stage->GetPrimAtPath(actorStatePath);
            if (!actorStatePrim) {
                printf("Failed to get actor state prim.\n");
                return;
            }

            UsdAttribute posAttr = actorStatePrim.GetAttribute(TfToken("pose:position"));
            UsdAttribute orientAttr = actorStatePrim.GetAttribute(TfToken("pose:orientation"));
            UsdAttribute scaleAttr = actorStatePrim.GetAttribute(TfToken("pose:scale"));

            PXR_NS::UsdGeomXformable destPrimXformable(actorPrim);
            bool resetXformStack = true;
            auto destXformOps = destPrimXformable.GetOrderedXformOps(&resetXformStack);
            if (destXformOps.size() == 0) {
                return;
            }

            //Connect pose data to xform ops in the destination prim
            for (auto& xformOp : destXformOps) {
                if (xformOp.GetOpType() == UsdGeomXformOp::TypeTranslate) {
                    xformOp.GetAttr().AddConnection(posAttr.GetPath());
                }
                else if (xformOp.GetOpType() == UsdGeomXformOp::TypeOrient) {
                    xformOp.GetAttr().AddConnection(orientAttr.GetPath());
                }
                else if (xformOp.GetOpType() == UsdGeomXformOp::TypeScale) {
                    xformOp.GetAttr().AddConnection(scaleAttr.GetPath());
                }
            }

            // Connect the effector pose data to the effector prim
            PXR_NS::SdfPath effectorsPath("/Components/effectors");
            effectorsPath = effectorsPath.AppendChild(TfToken(entityPrim.GetPath().GetName()));

            PXR_NS::UsdPrim effectorsPrim = m_stage->GetPrimAtPath(effectorsPath);
            if (!effectorsPrim) {
                printf("Skipping effector attribute connections for actor '%s': no effectors component found.\n", actorPath.GetText());
                continue;
            }

            for (const auto& effectorPrim : effectorsPrim.GetChildren()) {
                printf("Effector prim: %s\n", effectorPrim.GetPath().GetText());

                // Get effector relative path
                std::string relative_path;
                if (!effectorPrim.GetAttribute(TfToken("relative_path")).Get(&relative_path)) {
                    printf("Failed to get relative path attribute.\n");
                    continue;
                }

                // Get effector actor prim from base actor prim and relative path
                std::string combinedPath = std::string(actorPath.GetText()) + "/" + relative_path;
                SdfPath effectorActorPath(combinedPath);

                auto effectorActorPrim = m_stage->GetPrimAtPath(effectorActorPath);
                if (!effectorActorPrim) {
                    printf("Failed to get effector actor prim at path: %s\n", combinedPath.c_str());
                    continue;
                }

                UsdAttribute effectorPosAttr = effectorPrim.GetAttribute(TfToken("pose:position"));
                UsdAttribute effectorRotAttr = effectorPrim.GetAttribute(TfToken("pose:rotationXYZ"));
                UsdAttribute effectorScaleAttr = effectorPrim.GetAttribute(TfToken("pose:scale"));

                PXR_NS::UsdGeomXformable effectorXformable(effectorActorPrim);
                if (!effectorXformable) {
                    printf("Failed to get effector xformable.\n");
                    return;
                }

                bool resetXformStack = true;
                auto effectorsXformOps = effectorXformable.GetOrderedXformOps(&resetXformStack);
                if (effectorsXformOps.empty()) {
                    printf("Failed to get effector xform ops.\n");
                    return;
                }

                for (auto& xformOp : effectorsXformOps) {
                    if (xformOp.GetOpType() == UsdGeomXformOp::TypeTranslate) {
                        xformOp.GetAttr().AddConnection(effectorPosAttr.GetPath());
                    }
                    else if (xformOp.GetOpType() == UsdGeomXformOp::TypeRotateXYZ) {
                        xformOp.GetAttr().AddConnection(effectorRotAttr.GetPath());
                    }
                    else if (xformOp.GetOpType() == UsdGeomXformOp::TypeScale) {
                        xformOp.GetAttr().AddConnection(effectorScaleAttr.GetPath());
                    }
                }
            }
        }

    }

    void initializeSceneGraph() override
    {
        // It is important that all USD stage reads/writes happen from the main thread:
        // https ://graphics.pixar.com/usd/release/api/_usd__page__multi_threading.html
        if (!m_stage)
        {
            return;
        }

        initialize_logger("logs/aerosim_connector.log");
	    bool bIsMessageHandlerInitialized = initialize_message_handler("0", "zenoh");

	    if (bIsMessageHandlerInitialized)
	    {
	    	start_message_handler();
	    }
    }

    void removePrims() override
    {
        if (!m_stage)
        {
            return;
        }

        // Release all event subscriptions.
        PXR_NS::TfNotice::Revoke(m_usdNoticeListenerKey);
        m_updateEventsSubscription = nullptr;
    }

    void printStageInfo() const override
    {
        if (!m_stage)
        {
            return;
        }

        printf("---Stage Info Begin---\n");

        // Print the USD stage's up-axis.
        const PXR_NS::TfToken stageUpAxis = PXR_NS::UsdGeomGetStageUpAxis(m_stage);
        printf("Stage up-axis is: %s.\n", stageUpAxis.GetText());

        // Print the USD stage's meters per unit.
        const double metersPerUnit = PXR_NS::UsdGeomGetStageMetersPerUnit(m_stage);
        printf("Stage meters per unit: %f.\n", metersPerUnit);

        // Print the USD stage's prims.
        const PXR_NS::UsdPrimRange primRange = m_stage->Traverse();
        for (const PXR_NS::UsdPrim& prim : primRange)
        {
            printf("Stage contains prim: %s.\n", prim.GetPath().GetString().c_str());
        }

        printf("---Stage Info End---\n\n");
    }

    void onDefaultUsdStageChanged(long stageId) override
    {
        PXR_NS::TfNotice::Revoke(m_usdNoticeListenerKey);
        m_updateEventsSubscription = nullptr;
        m_stage.Reset();

        if (stageId)
        {
            m_stage = PXR_NS::UsdUtilsStageCache::Get().Find(PXR_NS::UsdStageCache::Id::FromLongInt(stageId));
            m_usdNoticeListenerKey = PXR_NS::TfNotice::Register(PXR_NS::TfCreateWeakPtr(this), &AerosimConnector::onObjectsChanged);
        }
    }

    void onObjectsChanged(const PXR_NS::UsdNotice::ObjectsChanged& objectsChanged)
    {
        // Check if any prims have been added or removed.

    }

    void updateAttributeConnections() {

        UsdPrim root = m_stage->GetPrimAtPath(SdfPath("/"));
        if(!root) {
            printf("Failed to get root prim.\n");
            return;
        }
        for (const UsdPrim& prim: UsdPrimRange(root)) {
            std::vector<UsdAttribute> attributes = prim.GetAttributes();

            for (const UsdAttribute& attr : attributes) {
                SdfPathVector connections;
                attr.GetConnections(&connections);
                for (const SdfPath& connection : connections) {

                    auto isSceneGraphConnection = connection.GetPrimPath().HasPrefix(SdfPath("/Components")) || connection.GetPrimPath().HasPrefix(SdfPath("/Resources"));
                    if (!isSceneGraphConnection) {
                        continue;
                    }

                    UsdPrim connectedPrim = m_stage->GetPrimAtPath(connection.GetPrimPath());
                    if (!connectedPrim) {
                        printf("Failed to get connected prim for attribute: %s\n", attr.GetName().GetText());
                        continue;
                    }

                    UsdAttribute connectedAttr = connectedPrim.GetAttribute(connection.GetNameToken());
                    if (!connectedAttr) {
                        printf("Failed to get connected attribute.\n");
                        continue;
                    }

                    VtValue value;
                    if (connectedAttr.Get(&value) && !value.IsEmpty()) {
                        attr.Set(value);
                    }
                }
            }
        }
    }

    bool isStopCommandReceived() override
    {
        return m_stopCommandReceived;
    }

    void clearStopCommandReceived() override
    {
        m_stopCommandReceived = false;
    }

    void onUpdateEvent() override
    {
        // It is important that all USD stage reads/writes happen from the main thread:
        // https ://graphics.pixar.com/usd/release/api/_usd__page__multi_threading.html
        if (!m_stage)
        {
            return;
        }

        // Query the message_handler for the latest scene_graph data
        char* sceneGraphData = nullptr;
        char* queueData = get_consumer_payload_from_queue();
        // Flush the queue to get only the latest data
        while (queueData != nullptr)
        {
            if (sceneGraphData != nullptr)
            {
                std::free(sceneGraphData);
            }
            sceneGraphData = queueData;
            queueData = get_consumer_payload_from_queue();
        }

        if (sceneGraphData != nullptr)
        {
            // Process scene_graph data
            // printf("Received scene graph data: %s\n", sceneGraphData);
            json sceneGraphJson;
            try {
                sceneGraphJson = json::parse(sceneGraphData);
            }
            catch (json::parse_error& e) {
                printf("Failed to parse scene_graph.json file: %s\n", e.what());
                std::free(sceneGraphData);
                return;
            }

            // Check if this is a stop command from the orchestrator
            if (sceneGraphJson.contains("command") && sceneGraphJson["command"] == "stop") {
                printf("[AerosimConnector] Received orchestrator stop command. Cleaning up...\n");

                // End the current message handler
                end_message_handler();

                // Reset scene graph state so the next connection triggers full initialization
                m_sceneGraphInitialized = false;
                m_usd_actor_prims.clear();

                // Set flag for Python to detect and reload the stage
                m_stopCommandReceived = true;

                printf("[AerosimConnector] Cleanup complete. Waiting for stage reload.\n");

                std::free(sceneGraphData);
                return;
            }

            processSceneGraphJson(sceneGraphJson);

            if (!m_sceneGraphInitialized)
            {
                // Remove Environment and load default sky
                PXR_NS::SdfPath envPath("/Environment");
                PXR_NS::UsdPrim envPrim = m_stage->GetPrimAtPath(envPath);
                if (envPrim) {
                    printf("Removing environment prim.\n");
                    m_stage->RemovePrim(envPrim.GetPath());
                } else {
                    printf("Failed to get environment prim.\n");
                }

                // Load default sky from root assets
                PXR_NS::SdfPath skyPath("/Environment");
                PXR_NS::UsdPrim skyPrim = m_stage->DefinePrim(skyPath);
                if (!skyPrim) {
                    printf("Failed to define sky prim.\n");
                    return;
                }

                // Load default sky from root assets
                std::string skyAsset = "environment/default_sky.usdc";
                std::string skyAssetPath = getAssetRootPath() + "/" + skyAsset;
                printf("Loading sky asset: %s\n", skyAssetPath.c_str());
                skyPrim.GetReferences().AddReference(skyAssetPath);

                printf("Initializing scene graph...\n");
                printf("Scene graph json file loaded successfully.\n");

                constructActorHierarchy();
                configureAttributeConnections();

                notify_scene_graph_loaded();
                m_sceneGraphInitialized = true;
            }
            updateAttributeConnections();

            std::free(sceneGraphData);
        }
    }

private:

    PXR_NS::UsdStageRefPtr m_stage;
    PXR_NS::TfNotice::Key m_usdNoticeListenerKey;
    carb::events::ISubscriptionPtr m_updateEventsSubscription;

    bool m_sceneGraphInitialized = false;
    bool m_stopCommandReceived = false;

    std::unordered_map<std::string, PXR_NS::UsdPrim> m_usd_actor_prims;
};
}
}

CARB_PLUGIN_IMPL(pluginImplDesc, aerosim::connector::AerosimConnector)

void fillInterface(aerosim::connector::AerosimConnector& iface)
{
}
