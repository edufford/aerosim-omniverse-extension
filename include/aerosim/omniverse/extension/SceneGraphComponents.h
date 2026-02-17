#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

struct Component {
    virtual ~Component() = default;
    std::string owner_entity_id;
    std::string component_type;
};

// ActorProperties
struct ActorProperties : Component {
    std::string actor_name;
    std::string actor_asset;
    std::string parent;

    friend void from_json(const json& j, ActorProperties& ap) {
        if (j.contains("actor_name")) j.at("actor_name").get_to(ap.actor_name);
        if (j.contains("actor_asset")) j.at("actor_asset").get_to(ap.actor_asset);
        if (j.contains("parent")) j.at("parent").get_to(ap.parent);
    }
};

// Vector Types
struct Vector2i {
    int x = 0, y = 0;
    friend void from_json(const json& j, Vector2i& v) {
        if (j.is_array() && j.size() >= 2) {
            // Rust serde serializes tuples as arrays: [1920, 1080]
            v.x = j[0].get<int>();
            v.y = j[1].get<int>();
        } else {
            if (j.contains("x")) j.at("x").get_to(v.x);
            if (j.contains("y")) j.at("y").get_to(v.y);
        }
    }
};

struct Vector3 {
    double x, y, z;
    friend void from_json(const json& j, Vector3& v) {
        if (j.contains("x")) j.at("x").get_to(v.x);
        if (j.contains("y")) j.at("y").get_to(v.y);
        if (j.contains("z")) j.at("z").get_to(v.z);
    }
};

struct Vector4 {
    double x, y, z, w;
    friend void from_json(const json& j, Vector4& v) {
        if (j.contains("x")) j.at("x").get_to(v.x);
        if (j.contains("y")) j.at("y").get_to(v.y);
        if (j.contains("z")) j.at("z").get_to(v.z);
        if (j.contains("w")) j.at("w").get_to(v.w);
    }
};

struct Transform {
    Vector3 position;
    Vector4 orientation;
    Vector3 scale;
    friend void from_json(const json& j, Transform& t) {
        if (j.contains("position")) j.at("position").get_to(t.position);
        if (j.contains("orientation")) j.at("orientation").get_to(t.orientation);
        if (j.contains("scale")) j.at("scale").get_to(t.scale);
    }
};

struct Pose {
    Transform transform;
    friend void from_json(const json& j, Pose& p) {
        if (j.contains("transform")) j.at("transform").get_to(p.transform);
    }
};

struct NED {
    double north, east, down;
    friend void from_json(const json& j, NED& n) {
        if (j.contains("north")) j.at("north").get_to(n.north);
        if (j.contains("east")) j.at("east").get_to(n.east);
        if (j.contains("down")) j.at("down").get_to(n.down);
    }
};

struct LLA {
    double latitude, longitude, altitude;
    friend void from_json(const json& j, LLA& l) {
        if (j.contains("latitude")) j.at("latitude").get_to(l.latitude);
        if (j.contains("longitude")) j.at("longitude").get_to(l.longitude);
        if (j.contains("altitude")) j.at("altitude").get_to(l.altitude);
    }
};

struct Ellipsoid {
    double equatorial_radius, flattening_factor, polar_radius;
    friend void from_json(const json& j, Ellipsoid& e) {
        if (j.contains("equatorial_radius")) j.at("equatorial_radius").get_to(e.equatorial_radius);
        if (j.contains("flattening_factor")) j.at("flattening_factor").get_to(e.flattening_factor);
        if (j.contains("polar_radius")) j.at("polar_radius").get_to(e.polar_radius);
    }
};

struct WorldCoordinate {
    NED ned;
    LLA lla;
    Vector3 ecef;
    Vector3 cartesian;
    LLA origin_lla;
    Ellipsoid ellipsoid;

    friend void from_json(const json& j, WorldCoordinate& wc) {
        if (j.contains("ned")) j.at("ned").get_to(wc.ned);
        if (j.contains("lla")) j.at("lla").get_to(wc.lla);
        if (j.contains("ecef")) j.at("ecef").get_to(wc.ecef);
        if (j.contains("cartesian")) j.at("cartesian").get_to(wc.cartesian);
        if (j.contains("origin_lla")) j.at("origin_lla").get_to(wc.origin_lla);
        if (j.contains("ellipsoid")) j.at("ellipsoid").get_to(wc.ellipsoid);
    }
};

struct ActorState : Component {
    Pose pose;
    WorldCoordinate worldCoordinate;
    friend void from_json(const json& j, ActorState& as) {
        if (j.contains("pose")) j.at("pose").get_to(as.pose);
        if (j.contains("world_coordinate")) j.at("world_coordinate").get_to(as.worldCoordinate);
    }
};

struct SensorParams {
    Vector2i resolution;
    double tick_rate = 0, fov = 0, near_clip = 0, far_clip = 0;
    bool capture_enabled = true;

    friend void from_json(const json& j, SensorParams& sp) {
        // Rust serde serializes the SensorParameters enum as {"RGBCamera": {...}}
        // or {"DepthSensor": {...}}. Unwrap the variant to get the actual parameters.
        const json* p = &j;
        if (j.contains("RGBCamera")) {
            p = &j.at("RGBCamera");
        } else if (j.contains("DepthSensor")) {
            p = &j.at("DepthSensor");
        }

        if (p->contains("resolution")) p->at("resolution").get_to(sp.resolution);
        if (p->contains("tick_rate")) p->at("tick_rate").get_to(sp.tick_rate);
        if (p->contains("fov")) p->at("fov").get_to(sp.fov);
        if (p->contains("near_clip")) p->at("near_clip").get_to(sp.near_clip);
        if (p->contains("far_clip")) p->at("far_clip").get_to(sp.far_clip);
        if (p->contains("capture_enabled")) p->at("capture_enabled").get_to(sp.capture_enabled);
    }
};

struct Sensor : Component {
    std::string sensor_name, sensor_type;
    SensorParams sensor_parameters;

    friend void from_json(const json& j, Sensor& s) {
        if (j.contains("sensor_name")) j.at("sensor_name").get_to(s.sensor_name);
        if (j.contains("sensor_type")) j.at("sensor_type").get_to(s.sensor_type);
        if (j.contains("sensor_parameters")) j.at("sensor_parameters").get_to(s.sensor_parameters);
    }
};

struct Effector {
    std::string effector_id, relative_path;
    Pose pose;

    friend void from_json(const json& j, Effector& e) {
        if (j.contains("effector_id")) j.at("effector_id").get_to(e.effector_id);
        if (j.contains("relative_path")) j.at("relative_path").get_to(e.relative_path);
        if (j.contains("pose")) j.at("pose").get_to(e.pose);
    }
};

struct Effectors : Component {
    std::vector<Effector> effectors;
    friend void from_json(const json& j, Effectors& e) {
        j.get_to(e.effectors);
    }
};