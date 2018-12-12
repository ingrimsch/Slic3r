#ifndef slic3r_SLAPrint_hpp_
#define slic3r_SLAPrint_hpp_

#include <mutex>

#include "PrintBase.hpp"
#include "PrintExport.hpp"
#include "Point.hpp"
#include "MTUtils.hpp"

namespace Slic3r {

enum SLAPrintStep : unsigned int {
	slapsRasterize,
	slapsValidate,
	slapsCount
};

enum SLAPrintObjectStep : unsigned int {
	slaposObjectSlice,
	slaposSupportIslands,
	slaposSupportPoints,
	slaposSupportTree,
	slaposBasePool,
	slaposSliceSupports,
    slaposIndexSlices,
	slaposCount
};

class SLAPrint;
class GLCanvas;

using _SLAPrintObjectBase =
    PrintObjectBaseWithState<SLAPrint, SLAPrintObjectStep, slaposCount>;

// Layers according to quantized height levels. This will be consumed by
// the printer (rasterizer) in the SLAPrint class.
using LevelID = long long;

class SLAPrintObject : public _SLAPrintObjectBase
{
private: // Prevents erroneous use by other classes.
    using Inherited = _SLAPrintObjectBase;

public:
    const SLAPrintObjectConfig& config() const { return m_config; }
    const Transform3d&          trafo()  const { return m_trafo; }

    struct Instance {
    	Instance(ModelID instance_id, const Point &shift, float rotation) : instance_id(instance_id), shift(shift), rotation(rotation) {}
		bool operator==(const Instance &rhs) const { return this->instance_id == rhs.instance_id && this->shift == rhs.shift && this->rotation == rhs.rotation; }
    	// ID of the corresponding ModelInstance.
		ModelID instance_id;
		// Slic3r::Point objects in scaled G-code coordinates
    	Point 	shift;
    	// Rotation along the Z axis, in radians.
    	float 	rotation;
	};
    const std::vector<Instance>& instances() const { return m_instances; }

    bool                    has_mesh(SLAPrintObjectStep step) const;
    TriangleMesh            get_mesh(SLAPrintObjectStep step) const;

    // Get a support mesh centered around origin in XY, and with zero rotation around Z applied.
    // Support mesh is only valid if this->is_step_done(slaposSupportTree) is true.
    const TriangleMesh&     support_mesh() const;
    // Get a pad mesh centered around origin in XY, and with zero rotation around Z applied.
    // Support mesh is only valid if this->is_step_done(slaposBasePool) is true.
    const TriangleMesh&     pad_mesh() const;

    // This will return the transformed mesh which is cached
    const TriangleMesh&     transformed_mesh() const;

    std::vector<Vec3d>      transformed_support_points() const;

    // Get the needed Z elevation for the model geometry if supports should be
    // displayed. This Z offset should also be applied to the support
    // geometries. Note that this is not the same as the value stored in config
    // as the pad height also needs to be considered.
    double get_elevation() const;

    // This method returns the needed elevation according to the processing
    // status. If the supports are not ready, it is zero, if they are and the
    // pad is not, then without the pad, otherwise the full value is returned.
    double get_current_elevation() const;

    // These two methods should be callable on the client side (e.g. UI thread)
    // when the appropriate steps slaposObjectSlice and slaposSliceSupports
    // are ready. All the print objects are processed before slapsRasterize so
    // it is safe to call them during and/or after slapsRasterize.
    const std::vector<ExPolygons>& get_model_slices() const;
    const std::vector<ExPolygons>& get_support_slices() const;

    // An index record referencing the slices
    // (get_model_slices(), get_support_slices()) where the keys are the height
    // levels of the model in scaled-clipper coordinates. The levels correspond
    // to the z coordinate of the object coordinate system.
    struct SliceRecord {
        using Key = float;

        using Idx = size_t;
        static const Idx NONE = Idx(-1); // this will be the max limit of size_t

        Idx model_slices_idx = NONE;
        Idx support_slices_idx = NONE;
    };

    using SliceIndex = std::map<SliceRecord::Key, SliceRecord>;

    // Retrieve the slice index which is readable only after slaposIndexSlices
    // is done.
    const SliceIndex& get_slice_index() const;

    // I refuse to grantee copying (Tamas)
    SLAPrintObject(const SLAPrintObject&) = delete;
    SLAPrintObject& operator=(const SLAPrintObject&) = delete;

protected:
    // to be called from SLAPrint only.
    friend class SLAPrint;

	SLAPrintObject(SLAPrint* print, ModelObject* model_object);
    ~SLAPrintObject();

    void                    config_apply(const ConfigBase &other, bool ignore_nonexistent = false) { this->m_config.apply(other, ignore_nonexistent); }
    void                    config_apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false) 
    	{ this->m_config.apply_only(other, keys, ignore_nonexistent); }

    void                    set_trafo(const Transform3d& trafo) {
        m_transformed_rmesh.invalidate([this, &trafo](){ m_trafo = trafo; });
    }

    void                    set_instances(const std::vector<Instance> &instances) { m_instances = instances; }
    // Invalidates the step, and its depending steps in SLAPrintObject and SLAPrint.
    bool                    invalidate_step(SLAPrintObjectStep step);
    bool                    invalidate_all_steps();
    // Invalidate steps based on a set of parameters changed.
    bool                    invalidate_state_by_config_options(const std::vector<t_config_option_key> &opt_keys);

private:
    // Object specific configuration, pulled from the configuration layer.
    SLAPrintObjectConfig                    m_config;
    // Translation in Z + Rotation by Y and Z + Scaling / Mirroring.
    Transform3d                             m_trafo = Transform3d::Identity();
    std::vector<Instance> 					m_instances;

    // Which steps have to be performed. Implicitly: all
    std::vector<bool>                       m_stepmask;
    std::vector<ExPolygons>                 m_model_slices;
    SliceIndex                              m_slice_index;
    std::vector<LevelID>                    m_level_ids;

    // Caching the transformed (m_trafo) raw mesh of the object
    mutable CachedObject<TriangleMesh>      m_transformed_rmesh;

    class SupportData;
    std::unique_ptr<SupportData> m_supportdata;
};

using PrintObjects = std::vector<SLAPrintObject*>;

class TriangleMesh;

/**
 * @brief This class is the high level FSM for the SLA printing process.
 *
 * It should support the background processing framework and contain the
 * metadata for the support geometries and their slicing. It should also
 * dispatch the SLA printing configuration values to the appropriate calculation
 * steps.
 */
class SLAPrint : public PrintBaseWithState<SLAPrintStep, slapsCount>
{
private: // Prevents erroneous use by other classes.
    typedef PrintBaseWithState<SLAPrintStep, slapsCount> Inherited;

public:
    SLAPrint(): m_stepmask(slapsCount, true) {}

	virtual ~SLAPrint() { this->clear(); }

	PrinterTechnology	technology() const noexcept { return ptSLA; }

    void                clear() override;
    bool                empty() const override { return m_objects.empty(); }
    ApplyStatus         apply(const Model &model, const DynamicPrintConfig &config) override;
    void                process() override;
    // Returns true if an object step is done on all objects and there's at least one object.    
    bool                is_step_done(SLAPrintObjectStep step) const;
    // Returns true if the last step was finished with success.
    bool                finished() const override { return this->is_step_done(slaposIndexSlices); }

    template<class Fmt> void export_raster(const std::string& fname) {
        if(m_printer) m_printer->save<Fmt>(fname);
    }
    const PrintObjects& objects() const { return m_objects; }

	std::string         output_filename() const override 
        { return this->PrintBase::output_filename(m_print_config.output_filename_format.value, "zip"); }

private:
    using SLAPrinter = FilePrinter<FilePrinterFormat::SLA_PNGZIP>;
    using SLAPrinterPtr = std::unique_ptr<SLAPrinter>;

    // Invalidate steps based on a set of parameters changed.
    bool invalidate_state_by_config_options(const std::vector<t_config_option_key> &opt_keys);

    SLAPrintConfig                  m_print_config;
    SLAPrinterConfig                m_printer_config;
    SLAMaterialConfig               m_material_config;
    SLAPrintObjectConfig            m_default_object_config;

    PrintObjects                    m_objects;
    std::vector<bool>               m_stepmask;

    // Definition of the print input map. It consists of the slices indexed
    // with scaled (clipper) Z coordinates. Also contains the instance
    // transformations in scaled and filtered version. This is enough for the
    // rasterizer to be able to draw every layer in the right position
    using Layer = ExPolygons;
    using LayerCopies = std::vector<SLAPrintObject::Instance>;
    struct LayerRef {
        std::reference_wrapper<const Layer> lref;
        std::reference_wrapper<const LayerCopies> copies;
        LayerRef(const Layer& lyr, const LayerCopies& cp) :
            lref(std::cref(lyr)), copies(std::cref(cp)) {}
    };

    // One level may contain multiple slices from multiple objects and their
    // supports
    using LayerRefs = std::vector<LayerRef>;
    std::map<LevelID, LayerRefs>            m_printer_input;

    // The printer itself
    SLAPrinterPtr                           m_printer;

	friend SLAPrintObject;
};

} // namespace Slic3r

#endif /* slic3r_SLAPrint_hpp_ */
