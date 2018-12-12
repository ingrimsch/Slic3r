#include "Model.hpp"
#include "Geometry.hpp"

#include "Format/AMF.hpp"
#include "Format/OBJ.hpp"
#include "Format/PRUS.hpp"
#include "Format/STL.hpp"
#include "Format/3mf.hpp"

#include <float.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "SVG.hpp"
#include <Eigen/Dense>

namespace Slic3r {

unsigned int Model::s_auto_extruder_id = 1;

size_t ModelBase::s_last_id = 0;

Model& Model::assign_copy(const Model &rhs)
{
    this->copy_id(rhs);
    // copy materials
    this->clear_materials();
    this->materials = rhs.materials;
    for (std::pair<const t_model_material_id, ModelMaterial*> &m : this->materials) {
        // Copy including the ID and m_model.
        m.second = new ModelMaterial(*m.second);
        m.second->set_model(this);
    }
    // copy objects
    this->clear_objects();
    this->objects.reserve(rhs.objects.size());
	for (const ModelObject *model_object : rhs.objects) {
        // Copy including the ID, leave ID set to invalid (zero).
        auto mo = ModelObject::new_copy(*model_object);
        mo->set_model(this);
		this->objects.emplace_back(mo);
    }
    return *this;
}

Model& Model::assign_copy(Model &&rhs)
{
    this->copy_id(rhs);
	// Move materials, adjust the parent pointer.
    this->clear_materials();
    this->materials = std::move(rhs.materials);
    for (std::pair<const t_model_material_id, ModelMaterial*> &m : this->materials)
        m.second->set_model(this);
    rhs.materials.clear();
    // Move objects, adjust the parent pointer.
    this->clear_objects();
	this->objects = std::move(rhs.objects);
    for (ModelObject *model_object : this->objects)
        model_object->set_model(this);
	rhs.objects.clear();
    return *this;
}

void Model::assign_new_unique_ids_recursive()
{
    this->set_new_unique_id();
    for (std::pair<const t_model_material_id, ModelMaterial*> &m : this->materials)
        m.second->assign_new_unique_ids_recursive();
    for (ModelObject *model_object : this->objects)
        model_object->assign_new_unique_ids_recursive();
}

Model Model::read_from_file(const std::string &input_file, DynamicPrintConfig *config, bool add_default_instances)
{
    Model model;

    DynamicPrintConfig temp_config;
    if (config == nullptr)
        config = &temp_config;

    bool result = false;
    if (boost::algorithm::iends_with(input_file, ".stl"))
        result = load_stl(input_file.c_str(), &model);
    else if (boost::algorithm::iends_with(input_file, ".obj"))
        result = load_obj(input_file.c_str(), &model);
    else if (!boost::algorithm::iends_with(input_file, ".zip.amf") && (boost::algorithm::iends_with(input_file, ".amf") ||
        boost::algorithm::iends_with(input_file, ".amf.xml")))
        result = load_amf(input_file.c_str(), config, &model);
    else if (boost::algorithm::iends_with(input_file, ".3mf"))
        result = load_3mf(input_file.c_str(), config, &model);
    else if (boost::algorithm::iends_with(input_file, ".prusa"))
        result = load_prus(input_file.c_str(), &model);
    else
        throw std::runtime_error("Unknown file format. Input file must have .stl, .obj, .amf(.xml) or .prusa extension.");

    if (! result)
        throw std::runtime_error("Loading of a model file failed.");

    if (model.objects.empty())
        throw std::runtime_error("The supplied file couldn't be read because it's empty");
    
    for (ModelObject *o : model.objects)
        o->input_file = input_file;
    
    if (add_default_instances)
        model.add_default_instances();

    return model;
}

Model Model::read_from_archive(const std::string &input_file, DynamicPrintConfig *config, bool add_default_instances)
{
    Model model;

    bool result = false;
    if (boost::algorithm::iends_with(input_file, ".3mf"))
        result = load_3mf(input_file.c_str(), config, &model);
    else if (boost::algorithm::iends_with(input_file, ".zip.amf"))
        result = load_amf(input_file.c_str(), config, &model);
    else
        throw std::runtime_error("Unknown file format. Input file must have .3mf or .zip.amf extension.");

    if (!result)
        throw std::runtime_error("Loading of a model file failed.");

    if (model.objects.empty())
        throw std::runtime_error("The supplied file couldn't be read because it's empty");

    for (ModelObject *o : model.objects)
    {
        if (boost::algorithm::iends_with(input_file, ".zip.amf"))
        {
            // we remove the .zip part of the extension to avoid it be added to filenames when exporting
            o->input_file = boost::ireplace_last_copy(input_file, ".zip.", ".");
        }
        else
            o->input_file = input_file;
    }

    if (add_default_instances)
        model.add_default_instances();

    return model;
}

void Model::repair()
{
    for (ModelObject *o : this->objects)
        o->repair();
}

ModelObject* Model::add_object()
{
    this->objects.emplace_back(new ModelObject(this));
    return this->objects.back();
}

ModelObject* Model::add_object(const char *name, const char *path, const TriangleMesh &mesh)
{
    ModelObject* new_object = new ModelObject(this);
    this->objects.push_back(new_object);
    new_object->name = name;
    new_object->input_file = path;
    ModelVolume *new_volume = new_object->add_volume(mesh);
    new_volume->name = name;
    new_object->invalidate_bounding_box();
    return new_object;
}

ModelObject* Model::add_object(const char *name, const char *path, TriangleMesh &&mesh)
{
    ModelObject* new_object = new ModelObject(this);
    this->objects.push_back(new_object);
    new_object->name = name;
    new_object->input_file = path;
    ModelVolume *new_volume = new_object->add_volume(std::move(mesh));
    new_volume->name = name;
    new_object->invalidate_bounding_box();
    return new_object;
}

ModelObject* Model::add_object(const ModelObject &other)
{
	ModelObject* new_object = ModelObject::new_clone(other);
    new_object->set_model(this);
    this->objects.push_back(new_object);
    return new_object;
}

void Model::delete_object(size_t idx)
{
    ModelObjectPtrs::iterator i = this->objects.begin() + idx;
    delete *i;
    this->objects.erase(i);
}

bool Model::delete_object(ModelObject* object)
{
    if (object != nullptr) {
        size_t idx = 0;
        for (ModelObject *model_object : objects) {
            if (model_object == object) {
                delete model_object;
                objects.erase(objects.begin() + idx);
                return true;
            }
            ++ idx;
        }
    }
    return false;
}

bool Model::delete_object(ModelID id)
{
    if (id.id != 0) {
        size_t idx = 0;
        for (ModelObject *model_object : objects) {
            if (model_object->id() == id) {
                delete model_object;
                objects.erase(objects.begin() + idx);
                return true;
            }
            ++ idx;
        }
    }
    return false;
}

void Model::clear_objects()
{
    for (ModelObject *o : this->objects)
        delete o;
    this->objects.clear();
}

void Model::delete_material(t_model_material_id material_id)
{
    ModelMaterialMap::iterator i = this->materials.find(material_id);
    if (i != this->materials.end()) {
        delete i->second;
        this->materials.erase(i);
    }
}

void Model::clear_materials()
{
    for (auto &m : this->materials)
        delete m.second;
    this->materials.clear();
}

ModelMaterial* Model::add_material(t_model_material_id material_id)
{
    assert(! material_id.empty());
    ModelMaterial* material = this->get_material(material_id);
    if (material == nullptr)
        material = this->materials[material_id] = new ModelMaterial(this);
    return material;
}

ModelMaterial* Model::add_material(t_model_material_id material_id, const ModelMaterial &other)
{
    assert(! material_id.empty());
    // delete existing material if any
    ModelMaterial* material = this->get_material(material_id);
    delete material;
    // set new material
	material = new ModelMaterial(other);
	material->set_model(this);
    this->materials[material_id] = material;
    return material;
}

// makes sure all objects have at least one instance
bool Model::add_default_instances()
{
    // apply a default position to all objects not having one
    for (ModelObject *o : this->objects)
        if (o->instances.empty())
            o->add_instance();
    return true;
}

// this returns the bounding box of the *transformed* instances
BoundingBoxf3 Model::bounding_box() const
{
    BoundingBoxf3 bb;
    for (ModelObject *o : this->objects)
        bb.merge(o->bounding_box());
    return bb;
}

unsigned int Model::update_print_volume_state(const BoundingBoxf3 &print_volume) 
{
    unsigned int num_printable = 0;
    for (ModelObject *model_object : this->objects)
        num_printable += model_object->check_instances_print_volume_state(print_volume);
    return num_printable;
}

bool Model::center_instances_around_point(const Vec2d &point)
{
    BoundingBoxf3 bb;
    for (ModelObject *o : this->objects)
        for (size_t i = 0; i < o->instances.size(); ++ i)
            bb.merge(o->instance_bounding_box(i, false));

    Vec2d shift2 = point - to_2d(bb.center());
	if (std::abs(shift2(0)) < EPSILON && std::abs(shift2(1)) < EPSILON)
		// No significant shift, don't do anything.
		return false;

	Vec3d shift3 = Vec3d(shift2(0), shift2(1), 0.0);
	for (ModelObject *o : this->objects) {
		for (ModelInstance *i : o->instances)
			i->set_offset(i->get_offset() + shift3);
		o->invalidate_bounding_box();
	}
	return true;
}

// flattens everything to a single mesh
TriangleMesh Model::mesh() const
{
    TriangleMesh mesh;
    for (const ModelObject *o : this->objects)
        mesh.merge(o->mesh());
    return mesh;
}

static bool _arrange(const Pointfs &sizes, coordf_t dist, const BoundingBoxf* bb, Pointfs &out)
{
    if (sizes.empty())
        // return if the list is empty or the following call to BoundingBoxf constructor will lead to a crash
        return true;

    // we supply unscaled data to arrange()
    bool result = Slic3r::Geometry::arrange(
        sizes.size(),               // number of parts
        BoundingBoxf(sizes).max,    // width and height of a single cell
        dist,                       // distance between cells
        bb,                         // bounding box of the area to fill
        out                         // output positions
    );

    if (!result && bb != nullptr) {
        // Try to arrange again ignoring bb
        result = Slic3r::Geometry::arrange(
            sizes.size(),               // number of parts
            BoundingBoxf(sizes).max,    // width and height of a single cell
            dist,                       // distance between cells
            nullptr,                    // bounding box of the area to fill
            out                         // output positions
        );
    }
    
    return result;
}

/*  arrange objects preserving their instance count
    but altering their instance positions */
bool Model::arrange_objects(coordf_t dist, const BoundingBoxf* bb)
{
    // get the (transformed) size of each instance so that we take
    // into account their different transformations when packing
    Pointfs instance_sizes;
    Pointfs instance_centers;
    for (const ModelObject *o : this->objects)
        for (size_t i = 0; i < o->instances.size(); ++ i) {
            // an accurate snug bounding box around the transformed mesh.
            BoundingBoxf3 bbox(o->instance_bounding_box(i, true));
            instance_sizes.emplace_back(to_2d(bbox.size()));
            instance_centers.emplace_back(to_2d(bbox.center()));
        }

    Pointfs positions;
    if (! _arrange(instance_sizes, dist, bb, positions))
        return false;

    size_t idx = 0;
    for (ModelObject *o : this->objects) {
        for (ModelInstance *i : o->instances) {
            Vec2d offset_xy = positions[idx] - instance_centers[idx];
            i->set_offset(Vec3d(offset_xy(0), offset_xy(1), i->get_offset(Z)));
            ++idx;
        }
        o->invalidate_bounding_box();
    }

    return true;
}

// Duplicate the entire model preserving instance relative positions.
void Model::duplicate(size_t copies_num, coordf_t dist, const BoundingBoxf* bb)
{
    Pointfs model_sizes(copies_num-1, to_2d(this->bounding_box().size()));
    Pointfs positions;
    if (! _arrange(model_sizes, dist, bb, positions))
        throw std::invalid_argument("Cannot duplicate part as the resulting objects would not fit on the print bed.\n");
    
    // note that this will leave the object count unaltered
    
    for (ModelObject *o : this->objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        for (const ModelInstance *i : instances) {
            for (const Vec2d &pos : positions) {
                ModelInstance *instance = o->add_instance(*i);
                instance->set_offset(instance->get_offset() + Vec3d(pos(0), pos(1), 0.0));
            }
        }
        o->invalidate_bounding_box();
    }
}

/*  this will append more instances to each object
    and then automatically rearrange everything */
void Model::duplicate_objects(size_t copies_num, coordf_t dist, const BoundingBoxf* bb)
{
    for (ModelObject *o : this->objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        for (const ModelInstance *i : instances)
            for (size_t k = 2; k <= copies_num; ++ k)
                o->add_instance(*i);
    }
    
    this->arrange_objects(dist, bb);
}

void Model::duplicate_objects_grid(size_t x, size_t y, coordf_t dist)
{
    if (this->objects.size() > 1) throw "Grid duplication is not supported with multiple objects";
    if (this->objects.empty()) throw "No objects!";

    ModelObject* object = this->objects.front();
    object->clear_instances();

    Vec3d ext_size = object->bounding_box().size() + dist * Vec3d::Ones();

    for (size_t x_copy = 1; x_copy <= x; ++x_copy) {
        for (size_t y_copy = 1; y_copy <= y; ++y_copy) {
            ModelInstance* instance = object->add_instance();
            instance->set_offset(Vec3d(ext_size(0) * (double)(x_copy - 1), ext_size(1) * (double)(y_copy - 1), 0.0));
        }
    }
}

bool Model::looks_like_multipart_object() const
{
    if (this->objects.size() <= 1)
        return false;
    double zmin = std::numeric_limits<double>::max();
    for (const ModelObject *obj : this->objects) {
        if (obj->volumes.size() > 1 || obj->config.keys().size() > 1)
            return false;
        for (const ModelVolume *vol : obj->volumes) {
            double zmin_this = vol->mesh.bounding_box().min(2);
            if (zmin == std::numeric_limits<double>::max())
                zmin = zmin_this;
            else if (std::abs(zmin - zmin_this) > EPSILON)
                // The volumes don't share zmin.
                return true;
        }
    }
    return false;
}

void Model::convert_multipart_object(unsigned int max_extruders)
{
    if (this->objects.empty())
        return;
    
    ModelObject* object = new ModelObject(this);
    object->input_file = this->objects.front()->input_file;
    object->name = this->objects.front()->name;
    //FIXME copy the config etc?

    reset_auto_extruder_id();

    for (const ModelObject* o : this->objects)
        for (const ModelVolume* v : o->volumes)
        {
            ModelVolume* new_v = object->add_volume(*v);
            if (new_v != nullptr)
            {
                new_v->name = o->name;
                new_v->config.set_deserialize("extruder", get_auto_extruder_id_as_string(max_extruders));
            }
        }

    for (const ModelInstance* i : this->objects.front()->instances)
        object->add_instance(*i);
    
    this->clear_objects();
    this->objects.push_back(object);
}

void Model::adjust_min_z()
{
    if (objects.empty())
        return;

    if (bounding_box().min(2) < 0.0)
    {
        for (ModelObject* obj : objects)
        {
            if (obj != nullptr)
            {
                coordf_t obj_min_z = obj->bounding_box().min(2);
                if (obj_min_z < 0.0)
                    obj->translate(0.0, 0.0, -obj_min_z);
            }
        }
    }
}

unsigned int Model::get_auto_extruder_id(unsigned int max_extruders)
{
    unsigned int id = s_auto_extruder_id;
    if (id > max_extruders) {
        // The current counter is invalid, likely due to switching the printer profiles
        // to a profile with a lower number of extruders.
        reset_auto_extruder_id();
        id = s_auto_extruder_id;
    } else if (++ s_auto_extruder_id > max_extruders) {
        reset_auto_extruder_id();
    }
    return id;
}

std::string Model::get_auto_extruder_id_as_string(unsigned int max_extruders)
{
    char str_extruder[64];
    sprintf(str_extruder, "%ud", get_auto_extruder_id(max_extruders));
    return str_extruder;
}

void Model::reset_auto_extruder_id()
{
    s_auto_extruder_id = 1;
}

std::string Model::propose_export_file_name() const
{
    for (const ModelObject *model_object : this->objects)
        for (ModelInstance *model_instance : model_object->instances)
            if (model_instance->is_printable())
                return model_object->input_file;
    return std::string();
}

ModelObject::~ModelObject()
{
    this->clear_volumes();
    this->clear_instances();
}

// maintains the m_model pointer
ModelObject& ModelObject::assign_copy(const ModelObject &rhs)
{
    this->copy_id(rhs);

    this->name                        = rhs.name;
    this->input_file                  = rhs.input_file;
    this->config                      = rhs.config;
    this->sla_support_points          = rhs.sla_support_points;
    this->layer_height_ranges         = rhs.layer_height_ranges;
    this->layer_height_profile        = rhs.layer_height_profile;
    this->layer_height_profile_valid  = rhs.layer_height_profile_valid;
    this->origin_translation          = rhs.origin_translation;
    m_bounding_box                    = rhs.m_bounding_box;
    m_bounding_box_valid              = rhs.m_bounding_box_valid;

    this->clear_volumes();
    this->volumes.reserve(rhs.volumes.size());
    for (ModelVolume *model_volume : rhs.volumes) {
        this->volumes.emplace_back(new ModelVolume(*model_volume));
        this->volumes.back()->set_model_object(this);
    }
    this->clear_instances();
	this->instances.reserve(rhs.instances.size());
    for (const ModelInstance *model_instance : rhs.instances) {
        this->instances.emplace_back(new ModelInstance(*model_instance));
        this->instances.back()->set_model_object(this);
    }

    return *this;
}

// maintains the m_model pointer
ModelObject& ModelObject::assign_copy(ModelObject &&rhs)
{
    this->copy_id(rhs);

    this->name                        = std::move(rhs.name);
    this->input_file                  = std::move(rhs.input_file);
    this->config                      = std::move(rhs.config);
    this->sla_support_points          = std::move(rhs.sla_support_points);
    this->layer_height_ranges         = std::move(rhs.layer_height_ranges);
    this->layer_height_profile        = std::move(rhs.layer_height_profile);
    this->layer_height_profile_valid  = std::move(rhs.layer_height_profile_valid);
    this->origin_translation          = std::move(rhs.origin_translation);
    m_bounding_box                    = std::move(rhs.m_bounding_box);
    m_bounding_box_valid              = std::move(rhs.m_bounding_box_valid);

    this->clear_volumes();
	this->volumes = std::move(rhs.volumes);
	rhs.volumes.clear();
    for (ModelVolume *model_volume : this->volumes)
        model_volume->set_model_object(this);
    this->clear_instances();
	this->instances = std::move(rhs.instances);
	rhs.instances.clear();
    for (ModelInstance *model_instance : this->instances)
        model_instance->set_model_object(this);

	return *this;
}

void ModelObject::assign_new_unique_ids_recursive()
{
    this->set_new_unique_id();
    for (ModelVolume *model_volume : this->volumes)
        model_volume->assign_new_unique_ids_recursive();
    for (ModelInstance *model_instance : this->instances)
        model_instance->assign_new_unique_ids_recursive();
}

// Clone this ModelObject including its volumes and instances, keep the IDs of the copies equal to the original.
// Called by Print::apply() to clone the Model / ModelObject hierarchy to the back end for background processing.
//ModelObject* ModelObject::clone(Model *parent)
//{
//    return new ModelObject(parent, *this, true);
//}

ModelVolume* ModelObject::add_volume(const TriangleMesh &mesh)
{
    ModelVolume* v = new ModelVolume(this, mesh);
    this->volumes.push_back(v);
    this->invalidate_bounding_box();
    return v;
}

ModelVolume* ModelObject::add_volume(TriangleMesh &&mesh)
{
    ModelVolume* v = new ModelVolume(this, std::move(mesh));
    this->volumes.push_back(v);
    this->invalidate_bounding_box();
    return v;
}

ModelVolume* ModelObject::add_volume(const ModelVolume &other)
{
    ModelVolume* v = new ModelVolume(this, other);
    this->volumes.push_back(v);
    this->invalidate_bounding_box();
    return v;
}

ModelVolume* ModelObject::add_volume(const ModelVolume &other, TriangleMesh &&mesh)
{
    ModelVolume* v = new ModelVolume(this, other, std::move(mesh));
    this->volumes.push_back(v);
    this->invalidate_bounding_box();
    return v;
}

void ModelObject::delete_volume(size_t idx)
{
    ModelVolumePtrs::iterator i = this->volumes.begin() + idx;
    delete *i;
    this->volumes.erase(i);

    if (this->volumes.size() == 1)
    {
        // only one volume left
        // center it and update the instances accordingly
        // rationale: the volume may be shifted with respect to the object center and this may lead to wrong rotation and scaling 
        // when modifying the instance matrix of the derived GLVolume
        ModelVolume* v = this->volumes.front();
        v->center_geometry();
        const Vec3d& vol_offset = v->get_offset();
        for (ModelInstance* inst : this->instances)
        {
            inst->set_offset(inst->get_offset() + inst->get_matrix(true) * vol_offset);
        }
        v->set_offset(Vec3d::Zero());
        v->set_new_unique_id();
    }

    this->invalidate_bounding_box();
}

void ModelObject::clear_volumes()
{
    for (ModelVolume *v : this->volumes)
        delete v;
    this->volumes.clear();
    this->invalidate_bounding_box();
}

ModelInstance* ModelObject::add_instance()
{
    ModelInstance* i = new ModelInstance(this);
    this->instances.push_back(i);
    this->invalidate_bounding_box();
    return i;
}

ModelInstance* ModelObject::add_instance(const ModelInstance &other)
{
    ModelInstance* i = new ModelInstance(this, other);
    this->instances.push_back(i);
    this->invalidate_bounding_box();
    return i;
}

ModelInstance* ModelObject::add_instance(const Vec3d &offset, const Vec3d &scaling_factor, const Vec3d &rotation)
{
    auto *instance = add_instance();
    instance->set_offset(offset);
    instance->set_scaling_factor(scaling_factor);
    instance->set_rotation(rotation);
    return instance;
}

void ModelObject::delete_instance(size_t idx)
{
    ModelInstancePtrs::iterator i = this->instances.begin() + idx;
    delete *i;
    this->instances.erase(i);
    this->invalidate_bounding_box();
}

void ModelObject::delete_last_instance()
{
    this->delete_instance(this->instances.size() - 1);
}

void ModelObject::clear_instances()
{
    for (ModelInstance *i : this->instances)
        delete i;
    this->instances.clear();
    this->invalidate_bounding_box();
}

// Returns the bounding box of the transformed instances.
// This bounding box is approximate and not snug.
const BoundingBoxf3& ModelObject::bounding_box() const
{
    if (! m_bounding_box_valid) {
        BoundingBoxf3 raw_bbox;
        for (const ModelVolume *v : this->volumes)
            if (v->is_model_part())
#if ENABLE_MODELVOLUME_TRANSFORM
            {
                TriangleMesh m = v->mesh;
                m.transform(v->get_matrix());
                raw_bbox.merge(m.bounding_box());
            }
#else
                // mesh.bounding_box() returns a cached value.
                raw_bbox.merge(v->mesh.bounding_box());
#endif // ENABLE_MODELVOLUME_TRANSFORM
        BoundingBoxf3 bb;
        for (const ModelInstance *i : this->instances)
            bb.merge(i->transform_bounding_box(raw_bbox));
        m_bounding_box = bb;
        m_bounding_box_valid = true;
    }
    return m_bounding_box;
}

// A mesh containing all transformed instances of this object.
TriangleMesh ModelObject::mesh() const
{
    TriangleMesh mesh;
    TriangleMesh raw_mesh = this->raw_mesh();
    for (const ModelInstance *i : this->instances) {
        TriangleMesh m = raw_mesh;
        i->transform_mesh(&m);
        mesh.merge(m);
    }
    return mesh;
}

// Non-transformed (non-rotated, non-scaled, non-translated) sum of non-modifier object volumes.
// Currently used by ModelObject::mesh(), to calculate the 2D envelope for 2D platter
// and to display the object statistics at ModelObject::print_info().
TriangleMesh ModelObject::raw_mesh() const
{
    TriangleMesh mesh;
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part())
#if ENABLE_MODELVOLUME_TRANSFORM
        {
            TriangleMesh vol_mesh(v->mesh);
            vol_mesh.transform(v->get_matrix());
            mesh.merge(vol_mesh);
        }
#else
        mesh.merge(v->mesh);
#endif // ENABLE_MODELVOLUME_TRANSFORM
    return mesh;
}

// A transformed snug bounding box around the non-modifier object volumes, without the translation applied.
// This bounding box is only used for the actual slicing.
BoundingBoxf3 ModelObject::raw_bounding_box() const
{
    BoundingBoxf3 bb;
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part()) {
            if (this->instances.empty())
                throw std::invalid_argument("Can't call raw_bounding_box() with no instances");

#if ENABLE_MODELVOLUME_TRANSFORM
            TriangleMesh vol_mesh(v->mesh);
            vol_mesh.transform(v->get_matrix());
            bb.merge(this->instances.front()->transform_mesh_bounding_box(vol_mesh, true));
#else
            bb.merge(this->instances.front()->transform_mesh_bounding_box(v->mesh, true));
#endif // ENABLE_MODELVOLUME_TRANSFORM
        }
    return bb;
}

// This returns an accurate snug bounding box of the transformed object instance, without the translation applied.
BoundingBoxf3 ModelObject::instance_bounding_box(size_t instance_idx, bool dont_translate) const
{
    BoundingBoxf3 bb;
#if ENABLE_MODELVOLUME_TRANSFORM
    for (ModelVolume *v : this->volumes)
    {
        if (v->is_model_part())
        {
            TriangleMesh mesh(v->mesh);
            mesh.transform(v->get_matrix());
            bb.merge(this->instances[instance_idx]->transform_mesh_bounding_box(mesh, dont_translate));
        }
    }
#else
    for (ModelVolume *v : this->volumes)
        if (v->is_model_part())
            bb.merge(this->instances[instance_idx]->transform_mesh_bounding_box(&v->mesh, dont_translate));
#endif // ENABLE_MODELVOLUME_TRANSFORM
    return bb;
}

void ModelObject::center_around_origin()
{
    // calculate the displacements needed to 
    // center this object around the origin
	BoundingBoxf3 bb;
	for (ModelVolume *v : this->volumes)
        if (v->is_model_part())
			bb.merge(v->mesh.bounding_box());
    
    // Shift is the vector from the center of the bounding box to the origin
    Vec3d shift = -bb.center();

    this->translate(shift);
    this->origin_translation += shift;

#if !ENABLE_MODELVOLUME_TRANSFORM
    if (!this->instances.empty()) {
        for (ModelInstance *i : this->instances) {
            i->set_offset(i->get_offset() - shift);
        }
        this->invalidate_bounding_box();
    }
#endif // !ENABLE_MODELVOLUME_TRANSFORM
}

void ModelObject::ensure_on_bed()
{
    translate_instances(Vec3d(0.0, 0.0, -get_min_z()));
}

void ModelObject::translate_instances(const Vec3d& vector)
{
    for (size_t i = 0; i < instances.size(); ++i)
    {
        translate_instance(i, vector);
    }
}

void ModelObject::translate_instance(size_t instance_idx, const Vec3d& vector)
{
    ModelInstance* i = instances[instance_idx];
    i->set_offset(i->get_offset() + vector);
    invalidate_bounding_box();
}

void ModelObject::translate(double x, double y, double z)
{
    for (ModelVolume *v : this->volumes)
    {
        v->translate(x, y, z);
    }

    if (m_bounding_box_valid)
        m_bounding_box.translate(x, y, z);
}

void ModelObject::scale(const Vec3d &versor)
{
    for (ModelVolume *v : this->volumes)
    {
        v->scale(versor);
    }
#if !ENABLE_MODELVOLUME_TRANSFORM
    // reset origin translation since it doesn't make sense anymore
    this->origin_translation = Vec3d::Zero();
#endif // !ENABLE_MODELVOLUME_TRANSFORM
    this->invalidate_bounding_box();
}

void ModelObject::rotate(double angle, Axis axis)
{
    for (ModelVolume *v : this->volumes)
    {
        v->rotate(angle, axis);
    }

    center_around_origin();

#if !ENABLE_MODELVOLUME_TRANSFORM
    this->origin_translation = Vec3d::Zero();
#endif // !ENABLE_MODELVOLUME_TRANSFORM
    this->invalidate_bounding_box();
}

void ModelObject::rotate(double angle, const Vec3d& axis)
{
    for (ModelVolume *v : this->volumes)
    {
        v->rotate(angle, axis);
    }

    center_around_origin();

#if !ENABLE_MODELVOLUME_TRANSFORM
    this->origin_translation = Vec3d::Zero();
#endif // !ENABLE_MODELVOLUME_TRANSFORM
    this->invalidate_bounding_box();
}

void ModelObject::mirror(Axis axis)
{
    for (ModelVolume *v : this->volumes)
    {
        v->mirror(axis);
    }

#if !ENABLE_MODELVOLUME_TRANSFORM
    this->origin_translation = Vec3d::Zero();
#endif // !ENABLE_MODELVOLUME_TRANSFORM
    this->invalidate_bounding_box();
}

size_t ModelObject::materials_count() const
{
    std::set<t_model_material_id> material_ids;
    for (const ModelVolume *v : this->volumes)
        material_ids.insert(v->material_id());
    return material_ids.size();
}

size_t ModelObject::facets_count() const
{
    size_t num = 0;
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part())
            num += v->mesh.stl.stats.number_of_facets;
    return num;
}

bool ModelObject::needed_repair() const
{
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part() && v->mesh.needed_repair())
            return true;
    return false;
}

ModelObjectPtrs ModelObject::cut(size_t instance, coordf_t z, bool keep_upper, bool keep_lower, bool rotate_lower)
{
    if (!keep_upper && !keep_lower) { return {}; }

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper = keep_upper ? ModelObject::new_clone(*this) : nullptr;
    ModelObject* lower = keep_lower ? ModelObject::new_clone(*this) : nullptr;

    if (keep_upper) {
        upper->set_model(nullptr);
        upper->sla_support_points.clear();
        upper->clear_volumes();
        upper->input_file = "";
    }

    if (keep_lower) {
        lower->set_model(nullptr);
        lower->sla_support_points.clear();
        lower->clear_volumes();
        lower->input_file = "";
    }

    // Because transformations are going to be applied to meshes directly,
    // we reset transformation of all instances and volumes,
    // except for translation and Z-rotation on instances, which are preserved
    // in the transformation matrix and not applied to the mesh transform.

    // const auto instance_matrix = instances[instance]->get_matrix(true);
    const auto instance_matrix = Geometry::assemble_transform(
        Vec3d::Zero(),  // don't apply offset
        instances[instance]->get_rotation().cwiseProduct(Vec3d(1.0, 1.0, 0.0)),   // don't apply Z-rotation
        instances[instance]->get_scaling_factor(),
        instances[instance]->get_mirror()
    );

    z -= instances[instance]->get_offset()(2);

    // Lower part per-instance bounding boxes
    std::vector<BoundingBoxf3> lower_bboxes { instances.size() };

    for (ModelVolume *volume : volumes) {
        const auto volume_matrix = volume->get_matrix();

        if (! volume->is_model_part()) {
            // Modifiers are not cut, but we still need to add the instance transformation
            // to the modifier volume transformation to preserve their shape properly.

            volume->set_transformation(Geometry::Transformation(instance_matrix * volume_matrix));

            if (keep_upper) { upper->add_volume(*volume); }
            if (keep_lower) { lower->add_volume(*volume); }
        } else {
            TriangleMesh upper_mesh, lower_mesh;

            // Transform the mesh by the combined transformation matrix
            volume->mesh.transform(instance_matrix * volume_matrix);

            // Perform cut
            TriangleMeshSlicer tms(&volume->mesh);
            tms.cut(z, &upper_mesh, &lower_mesh);

            // Reset volume transformation except for offset
            const Vec3d offset = volume->get_offset();
            volume->set_transformation(Geometry::Transformation());
            volume->set_offset(offset);

            if (keep_upper) {
                upper_mesh.repair();
                upper_mesh.reset_repair_stats();
            }
            if (keep_lower) {
                lower_mesh.repair();
                lower_mesh.reset_repair_stats();
            }

            if (keep_upper && upper_mesh.facets_count() > 0) {
                ModelVolume* vol    = upper->add_volume(upper_mesh);
                vol->name           = volume->name;
                vol->config         = volume->config;
                vol->set_material(volume->material_id(), *volume->material());
            }
            if (keep_lower && lower_mesh.facets_count() > 0) {
                ModelVolume* vol    = lower->add_volume(lower_mesh);
                vol->name           = volume->name;
                vol->config         = volume->config;
                vol->set_material(volume->material_id(), *volume->material());

                // Compute the lower part instances' bounding boxes to figure out where to place
                // the upper part
                if (keep_upper) {
                    for (size_t i = 0; i < instances.size(); i++) {
                        lower_bboxes[i].merge(instances[i]->transform_mesh_bounding_box(lower_mesh, true));
                    }
                }
            }
        }
    }

    ModelObjectPtrs res;

    if (keep_upper && upper->volumes.size() > 0) {
        upper->invalidate_bounding_box();
        upper->center_around_origin();

        // Reset instance transformation except offset and Z-rotation
        for (size_t i = 0; i < instances.size(); i++) {
            auto &instance = upper->instances[i];
            const Vec3d offset = instance->get_offset();
            const double rot_z = instance->get_rotation()(2);
            // The upper part displacement is set to half of the lower part bounding box
            // this is done in hope at least a part of the upper part will always be visible and draggable
            const Vec3d displace = lower_bboxes[i].size().cwiseProduct(Vec3d(-0.5, -0.5, 0.0));

            instance->set_transformation(Geometry::Transformation());
            instance->set_offset(offset + displace);
            instance->set_rotation(Vec3d(0.0, 0.0, rot_z));
        }

        res.push_back(upper);
    }
    if (keep_lower && lower->volumes.size() > 0) {
        lower->invalidate_bounding_box();
        lower->center_around_origin();

        // Reset instance transformation except offset and Z-rotation
        for (auto *instance : lower->instances) {
            const Vec3d offset = instance->get_offset();
            const double rot_z = instance->get_rotation()(2);

            instance->set_transformation(Geometry::Transformation());
            instance->set_offset(offset);
            instance->set_rotation(Vec3d(rotate_lower ? Geometry::deg2rad(180.0) : 0.0, 0.0, rot_z));
        }

        res.push_back(lower);
    }

    return res;
}

void ModelObject::split(ModelObjectPtrs* new_objects)
{
    if (this->volumes.size() > 1) {
        // We can't split meshes if there's more than one volume, because
        // we can't group the resulting meshes by object afterwards
        new_objects->emplace_back(this);
        return;
    }
    
    ModelVolume* volume = this->volumes.front();
    TriangleMeshPtrs meshptrs = volume->mesh.split();
    for (TriangleMesh *mesh : meshptrs) {
        mesh->repair();
        
        // XXX: this seems to be the only real usage of m_model, maybe refactor this so that it's not needed?
        ModelObject* new_object = m_model->add_object();    
		new_object->name   = this->name;
		new_object->config = this->config;
		new_object->instances.reserve(this->instances.size());
		for (const ModelInstance *model_instance : this->instances)
			new_object->add_instance(*model_instance);
#if ENABLE_MODELVOLUME_TRANSFORM
        ModelVolume* new_vol = new_object->add_volume(*volume, std::move(*mesh));
        new_vol->center_geometry();

        for (ModelInstance* model_instance : new_object->instances)
        {
            Vec3d shift = model_instance->get_transformation().get_matrix(true) * new_vol->get_offset();
            model_instance->set_offset(model_instance->get_offset() + shift);
        }

        new_vol->set_offset(Vec3d::Zero());
#else
        new_object->add_volume(*volume, std::move(*mesh));
#endif // ENABLE_MODELVOLUME_TRANSFORM
        new_objects->emplace_back(new_object);
        delete mesh;
    }
    
    return;
}

void ModelObject::repair()
{
    for (ModelVolume *v : this->volumes)
        v->mesh.repair();
}

double ModelObject::get_min_z() const
{
    if (instances.empty())
        return 0.0;
    else
    {
        double min_z = DBL_MAX;
        for (size_t i = 0; i < instances.size(); ++i)
        {
            min_z = std::min(min_z, get_instance_min_z(i));
        }
        return min_z;
    }
}

double ModelObject::get_instance_min_z(size_t instance_idx) const
{
    double min_z = DBL_MAX;

    ModelInstance* inst = instances[instance_idx];
    const Transform3d& mi = inst->get_matrix(true);

    for (const ModelVolume* v : volumes)
    {
        if (!v->is_model_part())
            continue;

#if ENABLE_MODELVOLUME_TRANSFORM
        Transform3d mv = mi * v->get_matrix();
        const TriangleMesh& hull = v->get_convex_hull();
        for (uint32_t f = 0; f < hull.stl.stats.number_of_facets; ++f)
        {
            const stl_facet* facet = hull.stl.facet_start + f;
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mv * facet->vertex[0].cast<double>()));
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mv * facet->vertex[1].cast<double>()));
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mv * facet->vertex[2].cast<double>()));
        }
#else
        for (uint32_t f = 0; f < v->mesh.stl.stats.number_of_facets; ++f)
        {
            const stl_facet* facet = v->mesh.stl.facet_start + f;
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mi * facet->vertex[0].cast<double>()));
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mi * facet->vertex[1].cast<double>()));
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mi * facet->vertex[2].cast<double>()));
        }
#endif // ENABLE_MODELVOLUME_TRANSFORM
    }

    return min_z + inst->get_offset(Z);
}

unsigned int ModelObject::check_instances_print_volume_state(const BoundingBoxf3& print_volume)
{
    unsigned int num_printable = 0;
    enum {
        INSIDE  = 1,
        OUTSIDE = 2
    };
    for (ModelInstance *model_instance : this->instances) {
        unsigned int inside_outside = 0;
        for (const ModelVolume *vol : this->volumes)
            if (vol->is_model_part()) {
#if ENABLE_MODELVOLUME_TRANSFORM
                BoundingBoxf3 bb = vol->get_convex_hull().transformed_bounding_box(model_instance->get_matrix() * vol->get_matrix());
#else
                BoundingBoxf3 bb = vol->get_convex_hull().transformed_bounding_box(model_instance->get_matrix());
#endif // ENABLE_MODELVOLUME_TRANSFORM
                if (print_volume.contains(bb))
                    inside_outside |= INSIDE;
                else if (print_volume.intersects(bb))
                    inside_outside |= INSIDE | OUTSIDE;
                else
                    inside_outside |= OUTSIDE;
            }
        model_instance->print_volume_state = 
            (inside_outside == (INSIDE | OUTSIDE)) ? ModelInstance::PVS_Partly_Outside :
            (inside_outside == INSIDE) ? ModelInstance::PVS_Inside : ModelInstance::PVS_Fully_Outside;
        if (inside_outside == INSIDE)
            ++ num_printable;
    }
    return num_printable;
}

void ModelObject::print_info() const
{
    using namespace std;
    cout << fixed;
    boost::nowide::cout << "[" << boost::filesystem::path(this->input_file).filename().string() << "]" << endl;
    
    TriangleMesh mesh = this->raw_mesh();
    mesh.check_topology();
    BoundingBoxf3 bb = mesh.bounding_box();
    Vec3d size = bb.size();
    cout << "size_x = " << size(0) << endl;
    cout << "size_y = " << size(1) << endl;
    cout << "size_z = " << size(2) << endl;
    cout << "min_x = " << bb.min(0) << endl;
    cout << "min_y = " << bb.min(1) << endl;
    cout << "min_z = " << bb.min(2) << endl;
    cout << "max_x = " << bb.max(0) << endl;
    cout << "max_y = " << bb.max(1) << endl;
    cout << "max_z = " << bb.max(2) << endl;
    cout << "number_of_facets = " << mesh.stl.stats.number_of_facets  << endl;
    cout << "manifold = "   << (mesh.is_manifold() ? "yes" : "no") << endl;
    
    mesh.repair();  // this calculates number_of_parts
    if (mesh.needed_repair()) {
        mesh.repair();
        if (mesh.stl.stats.degenerate_facets > 0)
            cout << "degenerate_facets = "  << mesh.stl.stats.degenerate_facets << endl;
        if (mesh.stl.stats.edges_fixed > 0)
            cout << "edges_fixed = "        << mesh.stl.stats.edges_fixed       << endl;
        if (mesh.stl.stats.facets_removed > 0)
            cout << "facets_removed = "     << mesh.stl.stats.facets_removed    << endl;
        if (mesh.stl.stats.facets_added > 0)
            cout << "facets_added = "       << mesh.stl.stats.facets_added      << endl;
        if (mesh.stl.stats.facets_reversed > 0)
            cout << "facets_reversed = "    << mesh.stl.stats.facets_reversed   << endl;
        if (mesh.stl.stats.backwards_edges > 0)
            cout << "backwards_edges = "    << mesh.stl.stats.backwards_edges   << endl;
    }
    cout << "number_of_parts =  " << mesh.stl.stats.number_of_parts << endl;
    cout << "volume = "           << mesh.volume()                  << endl;
}

void ModelVolume::set_material_id(t_model_material_id material_id)
{
    m_material_id = material_id;
    // ensure m_material_id references an existing material
    if (! material_id.empty())
        this->object->get_model()->add_material(material_id);
}

ModelMaterial* ModelVolume::material() const
{ 
    return this->object->get_model()->get_material(m_material_id);
}

void ModelVolume::set_material(t_model_material_id material_id, const ModelMaterial &material)
{
    m_material_id = material_id;
    if (! material_id.empty())
        this->object->get_model()->add_material(material_id, material);
}

// Extract the current extruder ID based on this ModelVolume's config and the parent ModelObject's config.
int ModelVolume::extruder_id() const
{
    int extruder_id = -1;
    if (this->is_model_part()) {
        const ConfigOption *opt = this->config.option("extruder");
        if (opt == nullptr)
            opt = this->object->config.option("extruder");
        extruder_id = (opt == nullptr) ? 0 : opt->getInt();
    }
    return extruder_id;
}

#if ENABLE_MODELVOLUME_TRANSFORM
void ModelVolume::center_geometry()
{
    Vec3d shift = -mesh.bounding_box().center();
    mesh.translate((float)shift(0), (float)shift(1), (float)shift(2));
    m_convex_hull.translate((float)shift(0), (float)shift(1), (float)shift(2));
    translate(-shift);
}
#endif // ENABLE_MODELVOLUME_TRANSFORM

void ModelVolume::calculate_convex_hull()
{
    m_convex_hull = mesh.convex_hull_3d();
}

const TriangleMesh& ModelVolume::get_convex_hull() const
{
    return m_convex_hull;
}

ModelVolume::Type ModelVolume::type_from_string(const std::string &s)
{
    // Legacy support
    if (s == "1")
        return PARAMETER_MODIFIER;
    // New type (supporting the support enforcers & blockers)
    if (s == "ModelPart")
        return MODEL_PART;
    if (s == "ParameterModifier")
        return PARAMETER_MODIFIER;
    if (s == "SupportEnforcer")
        return SUPPORT_ENFORCER;
    if (s == "SupportBlocker")
        return SUPPORT_BLOCKER;
    assert(s == "0");
    // Default value if invalud type string received.
    return MODEL_PART;
}

std::string ModelVolume::type_to_string(const Type t)
{
    switch (t) {
    case MODEL_PART:         return "ModelPart";
    case PARAMETER_MODIFIER: return "ParameterModifier";
    case SUPPORT_ENFORCER:   return "SupportEnforcer";
    case SUPPORT_BLOCKER:    return "SupportBlocker";
    default:
        assert(false);
        return "ModelPart";
    }
}

// Split this volume, append the result to the object owning this volume.
// Return the number of volumes created from this one.
// This is useful to assign different materials to different volumes of an object.
size_t ModelVolume::split(unsigned int max_extruders)
{
    TriangleMeshPtrs meshptrs = this->mesh.split();
    if (meshptrs.size() <= 1) {
        delete meshptrs.front();
        return 1;
    }

    size_t idx = 0;
    size_t ivolume = std::find(this->object->volumes.begin(), this->object->volumes.end(), this) - this->object->volumes.begin();
    std::string name = this->name;

    Model::reset_auto_extruder_id();
#if ENABLE_MODELVOLUME_TRANSFORM
    Vec3d offset = this->get_offset();
#endif // ENABLE_MODELVOLUME_TRANSFORM

    for (TriangleMesh *mesh : meshptrs) {
        mesh->repair();
        if (idx == 0)
        {
            this->mesh = std::move(*mesh);
            this->calculate_convex_hull();
            // Assign a new unique ID, so that a new GLVolume will be generated.
            this->set_new_unique_id();
        }
        else
            this->object->volumes.insert(this->object->volumes.begin() + (++ivolume), new ModelVolume(object, *this, std::move(*mesh)));

#if ENABLE_MODELVOLUME_TRANSFORM
        this->object->volumes[ivolume]->set_offset(Vec3d::Zero());
        this->object->volumes[ivolume]->center_geometry();
        this->object->volumes[ivolume]->translate(offset);
#endif // ENABLE_MODELVOLUME_TRANSFORM
        this->object->volumes[ivolume]->name = name + "_" + std::to_string(idx + 1);
        this->object->volumes[ivolume]->config.set_deserialize("extruder", Model::get_auto_extruder_id_as_string(max_extruders));
        delete mesh;
        ++ idx;
    }
    
    return idx;
}

void ModelVolume::translate(const Vec3d& displacement)
{
#if ENABLE_MODELVOLUME_TRANSFORM
    set_offset(get_offset() + displacement);
#else
    mesh.translate((float)displacement(0), (float)displacement(1), (float)displacement(2));
    m_convex_hull.translate((float)displacement(0), (float)displacement(1), (float)displacement(2));
#endif // ENABLE_MODELVOLUME_TRANSFORM
}

void ModelVolume::scale(const Vec3d& scaling_factors)
{
#if ENABLE_MODELVOLUME_TRANSFORM
    set_scaling_factor(get_scaling_factor().cwiseProduct(scaling_factors));
#else
    mesh.scale(scaling_factors);
    m_convex_hull.scale(scaling_factors);
#endif // ENABLE_MODELVOLUME_TRANSFORM
}

void ModelVolume::rotate(double angle, Axis axis)
{
#if ENABLE_MODELVOLUME_TRANSFORM
    switch (axis)
    {
    case X: { rotate(angle, Vec3d::UnitX()); break; }
    case Y: { rotate(angle, Vec3d::UnitY()); break; }
    case Z: { rotate(angle, Vec3d::UnitZ()); break; }
    }
#else
    mesh.rotate(angle, axis);
    m_convex_hull.rotate(angle, axis);
#endif // ENABLE_MODELVOLUME_TRANSFORM
}

void ModelVolume::rotate(double angle, const Vec3d& axis)
{
#if ENABLE_MODELVOLUME_TRANSFORM
    set_rotation(get_rotation() + Geometry::extract_euler_angles(Eigen::Quaterniond(Eigen::AngleAxisd(angle, axis)).toRotationMatrix()));
#else
    mesh.rotate(angle, axis);
    m_convex_hull.rotate(angle, axis);
#endif // ENABLE_MODELVOLUME_TRANSFORM
}

void ModelVolume::mirror(Axis axis)
{
#if ENABLE_MODELVOLUME_TRANSFORM
    Vec3d mirror = get_mirror();
    switch (axis)
    {
    case X: { mirror(0) *= -1.0; break; }
    case Y: { mirror(1) *= -1.0; break; }
    case Z: { mirror(2) *= -1.0; break; }
    }
    set_mirror(mirror);
#else
    mesh.mirror(axis);
    m_convex_hull.mirror(axis);
#endif // ENABLE_MODELVOLUME_TRANSFORM
}

#if !ENABLE_MODELVOLUME_TRANSFORM
void ModelInstance::set_rotation(const Vec3d& rotation)
{
    set_rotation(X, rotation(0));
    set_rotation(Y, rotation(1));
    set_rotation(Z, rotation(2));
}

void ModelInstance::set_rotation(Axis axis, double rotation)
{
    static const double TWO_PI = 2.0 * (double)PI;
    while (rotation < 0.0)
    {
        rotation += TWO_PI;
    }
    while (TWO_PI < rotation)
    {
        rotation -= TWO_PI;
    }
    m_rotation(axis) = rotation;
}

void ModelInstance::set_scaling_factor(const Vec3d& scaling_factor)
{
    set_scaling_factor(X, scaling_factor(0));
    set_scaling_factor(Y, scaling_factor(1));
    set_scaling_factor(Z, scaling_factor(2));
}

void ModelInstance::set_scaling_factor(Axis axis, double scaling_factor)
{
    m_scaling_factor(axis) = std::abs(scaling_factor);
}

void ModelInstance::set_mirror(const Vec3d& mirror)
{
    set_mirror(X, mirror(0));
    set_mirror(Y, mirror(1));
    set_mirror(Z, mirror(2));
}

void ModelInstance::set_mirror(Axis axis, double mirror)
{
    double abs_mirror = std::abs(mirror);
    if (abs_mirror == 0.0)
        mirror = 1.0;
    else if (abs_mirror != 1.0)
        mirror /= abs_mirror;

    m_mirror(axis) = mirror;
}
#endif // !ENABLE_MODELVOLUME_TRANSFORM

void ModelInstance::transform_mesh(TriangleMesh* mesh, bool dont_translate) const
{
    mesh->transform(get_matrix(dont_translate));
}

BoundingBoxf3 ModelInstance::transform_mesh_bounding_box(const TriangleMesh& mesh, bool dont_translate) const
{
    // Rotate around mesh origin.
    TriangleMesh copy(mesh);
    copy.transform(get_matrix(true, false, true, true));
    BoundingBoxf3 bbox = copy.bounding_box();

    if (!empty(bbox)) {
        // Scale the bounding box along the three axes.
        for (unsigned int i = 0; i < 3; ++i)
        {
#if ENABLE_MODELVOLUME_TRANSFORM
            if (std::abs(get_scaling_factor((Axis)i)-1.0) > EPSILON)
            {
                bbox.min(i) *= get_scaling_factor((Axis)i);
                bbox.max(i) *= get_scaling_factor((Axis)i);
#else
            if (std::abs(this->m_scaling_factor(i) - 1.0) > EPSILON)
            {
                bbox.min(i) *= this->m_scaling_factor(i);
                bbox.max(i) *= this->m_scaling_factor(i);
#endif // ENABLE_MODELVOLUME_TRANSFORM
            }
        }

        // Translate the bounding box.
        if (! dont_translate) {
#if ENABLE_MODELVOLUME_TRANSFORM
            bbox.min += get_offset();
            bbox.max += get_offset();
#else
            bbox.min += this->m_offset;
            bbox.max += this->m_offset;
#endif // ENABLE_MODELVOLUME_TRANSFORM
        }
    }
    return bbox;
}

BoundingBoxf3 ModelInstance::transform_bounding_box(const BoundingBoxf3 &bbox, bool dont_translate) const
{
    return bbox.transformed(get_matrix(dont_translate));
}

Vec3d ModelInstance::transform_vector(const Vec3d& v, bool dont_translate) const
{
    return get_matrix(dont_translate) * v;
}

void ModelInstance::transform_polygon(Polygon* polygon) const
{
#if ENABLE_MODELVOLUME_TRANSFORM
    // CHECK_ME -> Is the following correct or it should take in account all three rotations ?
    polygon->rotate(get_rotation(Z)); // rotate around polygon origin
    // CHECK_ME -> Is the following correct ?
    polygon->scale(get_scaling_factor(X), get_scaling_factor(Y)); // scale around polygon origin
#else
    // CHECK_ME -> Is the following correct or it should take in account all three rotations ?
    polygon->rotate(this->m_rotation(2));                // rotate around polygon origin
    // CHECK_ME -> Is the following correct ?
    polygon->scale(this->m_scaling_factor(0), this->m_scaling_factor(1));           // scale around polygon origin
#endif // ENABLE_MODELVOLUME_TRANSFORM
}

#if !ENABLE_MODELVOLUME_TRANSFORM
Transform3d ModelInstance::get_matrix(bool dont_translate, bool dont_rotate, bool dont_scale, bool dont_mirror) const
{
    Vec3d translation = dont_translate ? Vec3d::Zero() : m_offset;
    Vec3d rotation = dont_rotate ? Vec3d::Zero() : m_rotation;
    Vec3d scale = dont_scale ? Vec3d::Ones() : m_scaling_factor;
    Vec3d mirror = dont_mirror ? Vec3d::Ones() : m_mirror;
    return Geometry::assemble_transform(translation, rotation, scale, mirror);
}
#endif // !ENABLE_MODELVOLUME_TRANSFORM

// Test whether the two models contain the same number of ModelObjects with the same set of IDs
// ordered in the same order. In that case it is not necessary to kill the background processing.
bool model_object_list_equal(const Model &model_old, const Model &model_new)
{
    if (model_old.objects.size() != model_new.objects.size())
        return false;
    for (size_t i = 0; i < model_old.objects.size(); ++ i)
        if (model_old.objects[i]->id() != model_new.objects[i]->id())
            return false;
    return true;
}

// Test whether the new model is just an extension of the old model (new objects were added
// to the end of the original list. In that case it is not necessary to kill the background processing.
bool model_object_list_extended(const Model &model_old, const Model &model_new)
{
    if (model_old.objects.size() >= model_new.objects.size())
        return false;
    for (size_t i = 0; i < model_old.objects.size(); ++ i)
        if (model_old.objects[i]->id() != model_new.objects[i]->id())
            return false;
    return true;
}

bool model_volume_list_changed(const ModelObject &model_object_old, const ModelObject &model_object_new, const ModelVolume::Type type)
{
    bool modifiers_differ = false;
    size_t i_old, i_new;
    for (i_old = 0, i_new = 0; i_old < model_object_old.volumes.size() && i_new < model_object_new.volumes.size();) {
        const ModelVolume &mv_old = *model_object_old.volumes[i_old];
        const ModelVolume &mv_new = *model_object_new.volumes[i_new];
        if (mv_old.type() != type) {
            ++ i_old;
            continue;
        }
        if (mv_new.type() != type) {
            ++ i_new;
            continue;
        }
        if (mv_old.id() != mv_new.id())
            return true;
        //FIXME test for the content of the mesh!

#if ENABLE_MODELVOLUME_TRANSFORM
        if (!mv_old.get_matrix().isApprox(mv_new.get_matrix()))
            return true;
#endif // ENABLE_MODELVOLUME_TRANSFORM
        ++i_old;
        ++ i_new;
    }
    for (; i_old < model_object_old.volumes.size(); ++ i_old) {
        const ModelVolume &mv_old = *model_object_old.volumes[i_old];
        if (mv_old.type() == type)
            // ModelVolume was deleted.
            return true;
    }
    for (; i_new < model_object_new.volumes.size(); ++ i_new) {
        const ModelVolume &mv_new = *model_object_new.volumes[i_new];
        if (mv_new.type() == type)
            // ModelVolume was added.
            return true;
    }
    return false;
}

#ifdef _DEBUG
// Verify whether the IDs of Model / ModelObject / ModelVolume / ModelInstance / ModelMaterial are valid and unique.
void check_model_ids_validity(const Model &model)
{
    std::set<ModelID> ids;
    auto check = [&ids](ModelID id) { 
        assert(id.id > 0);
        assert(ids.find(id) == ids.end());
        ids.insert(id);
    };
    for (const ModelObject *model_object : model.objects) {
        check(model_object->id());
        for (const ModelVolume *model_volume : model_object->volumes)
            check(model_volume->id());
        for (const ModelInstance *model_instance : model_object->instances)
            check(model_instance->id());
    }
    for (const auto mm : model.materials)
        check(mm.second->id());
}

void check_model_ids_equal(const Model &model1, const Model &model2)
{
    // Verify whether the IDs of model1 and model match.
    assert(model1.objects.size() == model2.objects.size());
    for (size_t idx_model = 0; idx_model < model2.objects.size(); ++ idx_model) {
        const ModelObject &model_object1 = *model1.objects[idx_model];
        const ModelObject &model_object2 = *  model2.objects[idx_model];
        assert(model_object1.id() == model_object2.id());
        assert(model_object1.volumes.size() == model_object2.volumes.size());
        assert(model_object1.instances.size() == model_object2.instances.size());
        for (size_t i = 0; i < model_object1.volumes.size(); ++ i)
            assert(model_object1.volumes[i]->id() == model_object2.volumes[i]->id());
        for (size_t i = 0; i < model_object1.instances.size(); ++ i)
            assert(model_object1.instances[i]->id() == model_object2.instances[i]->id());
    }
    assert(model1.materials.size() == model2.materials.size());
    {
        auto it1 = model1.materials.begin();
        auto it2 = model2.materials.begin();
        for (; it1 != model1.materials.end(); ++ it1, ++ it2) {
            assert(it1->first == it2->first); // compare keys
            assert(it1->second->id() == it2->second->id());
        }
    }
}
#endif /* _DEBUG */

}
