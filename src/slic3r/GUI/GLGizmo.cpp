// Include GLGizmo.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmo.hpp"

#include <GL/glew.h>
#include <Eigen/Dense>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/SLA/SLASupportTree.hpp"

#include <cstdio>
#include <numeric>
#include <algorithm>

#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>
#include <wx/debug.h>

#include "Tab.hpp"
#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PresetBundle.hpp"

#include <wx/defs.h>

// TODO: Display tooltips quicker on Linux

static const float DEFAULT_BASE_COLOR[3] = { 0.625f, 0.625f, 0.625f };
static const float DEFAULT_DRAG_COLOR[3] = { 1.0f, 1.0f, 1.0f };
static const float DEFAULT_HIGHLIGHT_COLOR[3] = { 1.0f, 0.38f, 0.0f };

static const float AXES_COLOR[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

namespace Slic3r {
namespace GUI {

const float GLGizmoBase::Grabber::SizeFactor = 0.025f;
const float GLGizmoBase::Grabber::MinHalfSize = 1.5f;
const float GLGizmoBase::Grabber::DraggingScaleFactor = 1.25f;

GLGizmoBase::Grabber::Grabber()
    : center(Vec3d::Zero())
    , angles(Vec3d::Zero())
    , dragging(false)
    , enabled(true)
{
    color[0] = 1.0f;
    color[1] = 1.0f;
    color[2] = 1.0f;
}

void GLGizmoBase::Grabber::render(bool hover, float size) const
{
    float render_color[3];
    if (hover)
    {
        render_color[0] = 1.0f - color[0];
        render_color[1] = 1.0f - color[1];
        render_color[2] = 1.0f - color[2];
    }
    else
        ::memcpy((void*)render_color, (const void*)color, 3 * sizeof(float));

    render(size, render_color, true);
}

float GLGizmoBase::Grabber::get_half_size(float size) const
{
    return std::max(size * SizeFactor, MinHalfSize);
}

float GLGizmoBase::Grabber::get_dragging_half_size(float size) const
{
    return std::max(size * SizeFactor * DraggingScaleFactor, MinHalfSize);
}

void GLGizmoBase::Grabber::render(float size, const float* render_color, bool use_lighting) const
{
    float half_size = dragging ? get_dragging_half_size(size) : get_half_size(size);

    if (use_lighting)
        ::glEnable(GL_LIGHTING);

    ::glColor3fv(render_color);

    ::glPushMatrix();
    ::glTranslated(center(0), center(1), center(2));

    ::glRotated(Geometry::rad2deg(angles(2)), 0.0, 0.0, 1.0);
    ::glRotated(Geometry::rad2deg(angles(1)), 0.0, 1.0, 0.0);
    ::glRotated(Geometry::rad2deg(angles(0)), 1.0, 0.0, 0.0);

    // face min x
    ::glPushMatrix();
    ::glTranslatef(-(GLfloat)half_size, 0.0f, 0.0f);
    ::glRotatef(-90.0f, 0.0f, 1.0f, 0.0f);
    render_face(half_size);
    ::glPopMatrix();

    // face max x
    ::glPushMatrix();
    ::glTranslatef((GLfloat)half_size, 0.0f, 0.0f);
    ::glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
    render_face(half_size);
    ::glPopMatrix();

    // face min y
    ::glPushMatrix();
    ::glTranslatef(0.0f, -(GLfloat)half_size, 0.0f);
    ::glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    render_face(half_size);
    ::glPopMatrix();

    // face max y
    ::glPushMatrix();
    ::glTranslatef(0.0f, (GLfloat)half_size, 0.0f);
    ::glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
    render_face(half_size);
    ::glPopMatrix();

    // face min z
    ::glPushMatrix();
    ::glTranslatef(0.0f, 0.0f, -(GLfloat)half_size);
    ::glRotatef(180.0f, 1.0f, 0.0f, 0.0f);
    render_face(half_size);
    ::glPopMatrix();

    // face max z
    ::glPushMatrix();
    ::glTranslatef(0.0f, 0.0f, (GLfloat)half_size);
    render_face(half_size);
    ::glPopMatrix();

    ::glPopMatrix();

    if (use_lighting)
        ::glDisable(GL_LIGHTING);
}

void GLGizmoBase::Grabber::render_face(float half_size) const
{
    ::glBegin(GL_TRIANGLES);
    ::glNormal3f(0.0f, 0.0f, 1.0f);
    ::glVertex3f(-(GLfloat)half_size, -(GLfloat)half_size, 0.0f);
    ::glVertex3f((GLfloat)half_size, -(GLfloat)half_size, 0.0f);
    ::glVertex3f((GLfloat)half_size, (GLfloat)half_size, 0.0f);
    ::glVertex3f((GLfloat)half_size, (GLfloat)half_size, 0.0f);
    ::glVertex3f(-(GLfloat)half_size, (GLfloat)half_size, 0.0f);
    ::glVertex3f(-(GLfloat)half_size, -(GLfloat)half_size, 0.0f);
    ::glEnd();
}

GLGizmoBase::GLGizmoBase(GLCanvas3D& parent)
    : m_parent(parent)
    , m_group_id(-1)
    , m_state(Off)
    , m_shortcut_key(0)
    , m_hover_id(-1)
    , m_dragging(false)
#if ENABLE_IMGUI
    , m_imgui(wxGetApp().imgui())
#endif // ENABLE_IMGUI
{
    ::memcpy((void*)m_base_color, (const void*)DEFAULT_BASE_COLOR, 3 * sizeof(float));
    ::memcpy((void*)m_drag_color, (const void*)DEFAULT_DRAG_COLOR, 3 * sizeof(float));
    ::memcpy((void*)m_highlight_color, (const void*)DEFAULT_HIGHLIGHT_COLOR, 3 * sizeof(float));
}

void GLGizmoBase::set_hover_id(int id)
{
    if (m_grabbers.empty() || (id < (int)m_grabbers.size()))
    {
        m_hover_id = id;
        on_set_hover_id();
    }
}

void GLGizmoBase::set_highlight_color(const float* color)
{
    if (color != nullptr)
        ::memcpy((void*)m_highlight_color, (const void*)color, 3 * sizeof(float));
}

void GLGizmoBase::enable_grabber(unsigned int id)
{
    if ((0 <= id) && (id < (unsigned int)m_grabbers.size()))
        m_grabbers[id].enabled = true;

    on_enable_grabber(id);
}

void GLGizmoBase::disable_grabber(unsigned int id)
{
    if ((0 <= id) && (id < (unsigned int)m_grabbers.size()))
        m_grabbers[id].enabled = false;

    on_disable_grabber(id);
}

void GLGizmoBase::start_dragging(const GLCanvas3D::Selection& selection)
{
    m_dragging = true;

    for (int i = 0; i < (int)m_grabbers.size(); ++i)
    {
        m_grabbers[i].dragging = (m_hover_id == i);
    }

    on_start_dragging(selection);
}

void GLGizmoBase::stop_dragging()
{
    m_dragging = false;

    for (int i = 0; i < (int)m_grabbers.size(); ++i)
    {
        m_grabbers[i].dragging = false;
    }

    on_stop_dragging();
}

void GLGizmoBase::update(const UpdateData& data)
{
    if (m_hover_id != -1)
        on_update(data);
}

float GLGizmoBase::picking_color_component(unsigned int id) const
{
    int color = 254 - (int)id;
    if (m_group_id > -1)
        color -= m_group_id;

    return (float)color / 255.0f;
}

void GLGizmoBase::render_grabbers(const BoundingBoxf3& box) const
{
    float size = (float)box.max_size();

    for (int i = 0; i < (int)m_grabbers.size(); ++i)
    {
        if (m_grabbers[i].enabled)
            m_grabbers[i].render((m_hover_id == i), size);
    }
}

void GLGizmoBase::render_grabbers(float size) const
{
    for (int i = 0; i < (int)m_grabbers.size(); ++i)
    {
        if (m_grabbers[i].enabled)
            m_grabbers[i].render((m_hover_id == i), size);
    }
}

void GLGizmoBase::render_grabbers_for_picking(const BoundingBoxf3& box) const
{
    float size = (float)box.max_size();

    for (unsigned int i = 0; i < (unsigned int)m_grabbers.size(); ++i)
    {
        if (m_grabbers[i].enabled)
        {
            m_grabbers[i].color[0] = 1.0f;
            m_grabbers[i].color[1] = 1.0f;
            m_grabbers[i].color[2] = picking_color_component(i);
            m_grabbers[i].render_for_picking(size);
        }
    }
}

#if !ENABLE_IMGUI
void GLGizmoBase::create_external_gizmo_widgets(wxWindow *parent) {}
#endif // not ENABLE_IMGUI

void GLGizmoBase::set_tooltip(const std::string& tooltip) const
{
    m_parent.set_tooltip(tooltip);
}

std::string GLGizmoBase::format(float value, unsigned int decimals) const
{
    return Slic3r::string_printf("%.*f", decimals, value);
}

const float GLGizmoRotate::Offset = 5.0f;
const unsigned int GLGizmoRotate::CircleResolution = 64;
const unsigned int GLGizmoRotate::AngleResolution = 64;
const unsigned int GLGizmoRotate::ScaleStepsCount = 72;
const float GLGizmoRotate::ScaleStepRad = 2.0f * (float)PI / GLGizmoRotate::ScaleStepsCount;
const unsigned int GLGizmoRotate::ScaleLongEvery = 2;
const float GLGizmoRotate::ScaleLongTooth = 0.1f; // in percent of radius
const unsigned int GLGizmoRotate::SnapRegionsCount = 8;
const float GLGizmoRotate::GrabberOffset = 0.15f; // in percent of radius

GLGizmoRotate::GLGizmoRotate(GLCanvas3D& parent, GLGizmoRotate::Axis axis)
    : GLGizmoBase(parent)
    , m_axis(axis)
    , m_angle(0.0)
    , m_quadric(nullptr)
    , m_center(0.0, 0.0, 0.0)
    , m_radius(0.0f)
    , m_snap_coarse_in_radius(0.0f)
    , m_snap_coarse_out_radius(0.0f)
    , m_snap_fine_in_radius(0.0f)
    , m_snap_fine_out_radius(0.0f)
{
    m_quadric = ::gluNewQuadric();
    if (m_quadric != nullptr)
        ::gluQuadricDrawStyle(m_quadric, GLU_FILL);
}

GLGizmoRotate::GLGizmoRotate(const GLGizmoRotate& other)
    : GLGizmoBase(other.m_parent)
    , m_axis(other.m_axis)
    , m_angle(other.m_angle)
    , m_quadric(nullptr)
    , m_center(other.m_center)
    , m_radius(other.m_radius)
    , m_snap_coarse_in_radius(other.m_snap_coarse_in_radius)
    , m_snap_coarse_out_radius(other.m_snap_coarse_out_radius)
    , m_snap_fine_in_radius(other.m_snap_fine_in_radius)
    , m_snap_fine_out_radius(other.m_snap_fine_out_radius)
{
    m_quadric = ::gluNewQuadric();
    if (m_quadric != nullptr)
        ::gluQuadricDrawStyle(m_quadric, GLU_FILL);
}

GLGizmoRotate::~GLGizmoRotate()
{
    if (m_quadric != nullptr)
        ::gluDeleteQuadric(m_quadric);
}

void GLGizmoRotate::set_angle(double angle)
{
    if (std::abs(angle - 2.0 * (double)PI) < EPSILON)
        angle = 0.0;

    m_angle = angle;
}

bool GLGizmoRotate::on_init()
{
    m_grabbers.push_back(Grabber());
    return true;
}

void GLGizmoRotate::on_start_dragging(const GLCanvas3D::Selection& selection)
{
    const BoundingBoxf3& box = selection.get_bounding_box();
    m_center = box.center();
    m_radius = Offset + box.radius();
    m_snap_coarse_in_radius = m_radius / 3.0f;
    m_snap_coarse_out_radius = 2.0f * m_snap_coarse_in_radius;
    m_snap_fine_in_radius = m_radius;
    m_snap_fine_out_radius = m_snap_fine_in_radius + m_radius * ScaleLongTooth;
}

void GLGizmoRotate::on_update(const UpdateData& data)
{
    Vec2d mouse_pos = to_2d(mouse_position_in_local_plane(data.mouse_ray));

    Vec2d orig_dir = Vec2d::UnitX();
    Vec2d new_dir = mouse_pos.normalized();

    double theta = ::acos(clamp(-1.0, 1.0, new_dir.dot(orig_dir)));
    if (cross2(orig_dir, new_dir) < 0.0)
        theta = 2.0 * (double)PI - theta;

    double len = mouse_pos.norm();

    // snap to coarse snap region
    if ((m_snap_coarse_in_radius <= len) && (len <= m_snap_coarse_out_radius))
    {
        double step = 2.0 * (double)PI / (double)SnapRegionsCount;
        theta = step * (double)std::round(theta / step);
    }
    else
    {
        // snap to fine snap region (scale)
        if ((m_snap_fine_in_radius <= len) && (len <= m_snap_fine_out_radius))
        {
            double step = 2.0 * (double)PI / (double)ScaleStepsCount;
            theta = step * (double)std::round(theta / step);
        }
    }

    if (theta == 2.0 * (double)PI)
        theta = 0.0;

    m_angle = theta;
}

void GLGizmoRotate::on_render(const GLCanvas3D::Selection& selection) const
{
    if (!m_grabbers[0].enabled)
        return;

    const BoundingBoxf3& box = selection.get_bounding_box();
#if !ENABLE_WORLD_ROTATIONS
    bool single_selection = selection.is_single_full_instance() || selection.is_single_modifier() || selection.is_single_volume();
#endif // !ENABLE_WORLD_ROTATIONS

    std::string axis;
    switch (m_axis)
    {
    case X: { axis = "X"; break; }
    case Y: { axis = "Y"; break; }
    case Z: { axis = "Z"; break; }
    }

#if ENABLE_WORLD_ROTATIONS
    if (!m_dragging && (m_hover_id == 0))
        set_tooltip(axis);
    else if (m_dragging)
#else
    if ((single_selection && (m_hover_id == 0)) || m_dragging)
#endif // ENABLE_WORLD_ROTATIONS
        set_tooltip(axis + ": " + format((float)Geometry::rad2deg(m_angle), 4) + "\u00B0");
    else
    {
        m_center = box.center();
        m_radius = Offset + box.radius();
        m_snap_coarse_in_radius = m_radius / 3.0f;
        m_snap_coarse_out_radius = 2.0f * m_snap_coarse_in_radius;
        m_snap_fine_in_radius = m_radius;
        m_snap_fine_out_radius = m_radius * (1.0f + ScaleLongTooth);
    }

    ::glEnable(GL_DEPTH_TEST);

    ::glPushMatrix();
    transform_to_local();

    ::glLineWidth((m_hover_id != -1) ? 2.0f : 1.5f);
    ::glColor3fv((m_hover_id != -1) ? m_drag_color : m_highlight_color);

    render_circle();

    if (m_hover_id != -1)
    {
        render_scale();
        render_snap_radii();
        render_reference_radius();
    }

    ::glColor3fv(m_highlight_color);

    if (m_hover_id != -1)
        render_angle();

    render_grabber(box);
    render_grabber_extension(box, false);

    ::glPopMatrix();
}

void GLGizmoRotate::on_render_for_picking(const GLCanvas3D::Selection& selection) const
{
    ::glDisable(GL_DEPTH_TEST);

    ::glPushMatrix();

    transform_to_local();

    const BoundingBoxf3& box = selection.get_bounding_box();
    render_grabbers_for_picking(box);
    render_grabber_extension(box, true);

    ::glPopMatrix();
}

void GLGizmoRotate::render_circle() const
{
    ::glBegin(GL_LINE_LOOP);
    for (unsigned int i = 0; i < ScaleStepsCount; ++i)
    {
        float angle = (float)i * ScaleStepRad;
        float x = ::cos(angle) * m_radius;
        float y = ::sin(angle) * m_radius;
        float z = 0.0f;
        ::glVertex3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
    }
    ::glEnd();
}

void GLGizmoRotate::render_scale() const
{
    float out_radius_long = m_snap_fine_out_radius;
    float out_radius_short = m_radius * (1.0f + 0.5f * ScaleLongTooth);

    ::glBegin(GL_LINES);
    for (unsigned int i = 0; i < ScaleStepsCount; ++i)
    {
        float angle = (float)i * ScaleStepRad;
        float cosa = ::cos(angle);
        float sina = ::sin(angle);
        float in_x = cosa * m_radius;
        float in_y = sina * m_radius;
        float in_z = 0.0f;
        float out_x = (i % ScaleLongEvery == 0) ? cosa * out_radius_long : cosa * out_radius_short;
        float out_y = (i % ScaleLongEvery == 0) ? sina * out_radius_long : sina * out_radius_short;
        float out_z = 0.0f;
        ::glVertex3f((GLfloat)in_x, (GLfloat)in_y, (GLfloat)in_z);
        ::glVertex3f((GLfloat)out_x, (GLfloat)out_y, (GLfloat)out_z);
    }
    ::glEnd();
}

void GLGizmoRotate::render_snap_radii() const
{
    float step = 2.0f * (float)PI / (float)SnapRegionsCount;

    float in_radius = m_radius / 3.0f;
    float out_radius = 2.0f * in_radius;

    ::glBegin(GL_LINES);
    for (unsigned int i = 0; i < SnapRegionsCount; ++i)
    {
        float angle = (float)i * step;
        float cosa = ::cos(angle);
        float sina = ::sin(angle);
        float in_x = cosa * in_radius;
        float in_y = sina * in_radius;
        float in_z = 0.0f;
        float out_x = cosa * out_radius;
        float out_y = sina * out_radius;
        float out_z = 0.0f;
        ::glVertex3f((GLfloat)in_x, (GLfloat)in_y, (GLfloat)in_z);
        ::glVertex3f((GLfloat)out_x, (GLfloat)out_y, (GLfloat)out_z);
    }
    ::glEnd();
}

void GLGizmoRotate::render_reference_radius() const
{
    ::glBegin(GL_LINES);
    ::glVertex3f(0.0f, 0.0f, 0.0f);
    ::glVertex3f((GLfloat)(m_radius * (1.0f + GrabberOffset)), 0.0f, 0.0f);
    ::glEnd();
}

void GLGizmoRotate::render_angle() const
{
    float step_angle = (float)m_angle / AngleResolution;
    float ex_radius = m_radius * (1.0f + GrabberOffset);

    ::glBegin(GL_LINE_STRIP);
    for (unsigned int i = 0; i <= AngleResolution; ++i)
    {
        float angle = (float)i * step_angle;
        float x = ::cos(angle) * ex_radius;
        float y = ::sin(angle) * ex_radius;
        float z = 0.0f;
        ::glVertex3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
    }
    ::glEnd();
}

void GLGizmoRotate::render_grabber(const BoundingBoxf3& box) const
{
#if ENABLE_WORLD_ROTATIONS
    double grabber_radius = (double)m_radius * (1.0 + (double)GrabberOffset);
#else
    double grabber_radius = (double)m_radius * (1.0 + (double)GrabberOffset) + 2.0 * (double)m_axis * (double)m_grabbers[0].get_half_size((float)box.max_size());
#endif // ENABLE_WORLD_ROTATIONS
    m_grabbers[0].center = Vec3d(::cos(m_angle) * grabber_radius, ::sin(m_angle) * grabber_radius, 0.0);
    m_grabbers[0].angles(2) = m_angle;

    ::glColor3fv((m_hover_id != -1) ? m_drag_color : m_highlight_color);

    ::glBegin(GL_LINES);
    ::glVertex3f(0.0f, 0.0f, 0.0f);
    ::glVertex3dv(m_grabbers[0].center.data());
    ::glEnd();

    ::memcpy((void*)m_grabbers[0].color, (const void*)m_highlight_color, 3 * sizeof(float));
    render_grabbers(box);
}

void GLGizmoRotate::render_grabber_extension(const BoundingBoxf3& box, bool picking) const
{
    if (m_quadric == nullptr)
        return;

    double size = m_dragging ? (double)m_grabbers[0].get_dragging_half_size((float)box.max_size()) : (double)m_grabbers[0].get_half_size((float)box.max_size());

    float color[3];
    ::memcpy((void*)color, (const void*)m_grabbers[0].color, 3 * sizeof(float));
    if (!picking && (m_hover_id != -1))
    {
        color[0] = 1.0f - color[0];
        color[1] = 1.0f - color[1];
        color[2] = 1.0f - color[2];
    }

    if (!picking)
        ::glEnable(GL_LIGHTING);

    ::glColor3fv(color);
    ::glPushMatrix();
    ::glTranslated(m_grabbers[0].center(0), m_grabbers[0].center(1), m_grabbers[0].center(2));
    ::glRotated(Geometry::rad2deg(m_angle), 0.0, 0.0, 1.0);
    ::glRotated(90.0, 1.0, 0.0, 0.0);
    ::glTranslated(0.0, 0.0, 2.0 * size);
    ::gluQuadricOrientation(m_quadric, GLU_OUTSIDE);
    ::gluCylinder(m_quadric, 0.75 * size, 0.0, 3.0 * size, 36, 1);
    ::gluQuadricOrientation(m_quadric, GLU_INSIDE);
    ::gluDisk(m_quadric, 0.0, 0.75 * size, 36, 1);
    ::glPopMatrix();
    ::glPushMatrix();
    ::glTranslated(m_grabbers[0].center(0), m_grabbers[0].center(1), m_grabbers[0].center(2));
    ::glRotated(Geometry::rad2deg(m_angle), 0.0, 0.0, 1.0);
    ::glRotated(-90.0, 1.0, 0.0, 0.0);
    ::glTranslated(0.0, 0.0, 2.0 * size);
    ::gluQuadricOrientation(m_quadric, GLU_OUTSIDE);
    ::gluCylinder(m_quadric, 0.75 * size, 0.0, 3.0 * size, 36, 1);
    ::gluQuadricOrientation(m_quadric, GLU_INSIDE);
    ::gluDisk(m_quadric, 0.0, 0.75 * size, 36, 1);
    ::glPopMatrix();

    if (!picking)
        ::glDisable(GL_LIGHTING);
}

void GLGizmoRotate::transform_to_local() const
{
    ::glTranslated(m_center(0), m_center(1), m_center(2));

    switch (m_axis)
    {
    case X:
    {
        ::glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
        ::glRotatef(-90.0f, 0.0f, 0.0f, 1.0f);
        break;
    }
    case Y:
    {
        ::glRotatef(-90.0f, 0.0f, 0.0f, 1.0f);
        ::glRotatef(-90.0f, 0.0f, 1.0f, 0.0f);
        break;
    }
    default:
    case Z:
    {
        // no rotation
        break;
    }
    }
}

Vec3d GLGizmoRotate::mouse_position_in_local_plane(const Linef3& mouse_ray) const
{
    double half_pi = 0.5 * (double)PI;

    Transform3d m = Transform3d::Identity();

    switch (m_axis)
    {
    case X:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        m.rotate(Eigen::AngleAxisd(-half_pi, Vec3d::UnitY()));
        break;
    }
    case Y:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitY()));
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        break;
    }
    default:
    case Z:
    {
        // no rotation applied
        break;
    }
    }

    m.translate(-m_center);

    return transform(mouse_ray, m).intersect_plane(0.0);
}

GLGizmoRotate3D::GLGizmoRotate3D(GLCanvas3D& parent)
    : GLGizmoBase(parent)
{
    m_gizmos.emplace_back(parent, GLGizmoRotate::X);
    m_gizmos.emplace_back(parent, GLGizmoRotate::Y);
    m_gizmos.emplace_back(parent, GLGizmoRotate::Z);

    for (unsigned int i = 0; i < 3; ++i)
    {
        m_gizmos[i].set_group_id(i);
    }
}

bool GLGizmoRotate3D::on_init()
{
    for (GLGizmoRotate& g : m_gizmos)
    {
        if (!g.init())
            return false;
    }

    for (unsigned int i = 0; i < 3; ++i)
    {
        m_gizmos[i].set_highlight_color(AXES_COLOR[i]);
    }

    std::string path = resources_dir() + "/icons/overlay/";

    if (!m_textures[Off].load_from_file(path + "rotate_off.png", false))
        return false;

    if (!m_textures[Hover].load_from_file(path + "rotate_hover.png", false))
        return false;

    if (!m_textures[On].load_from_file(path + "rotate_on.png", false))
        return false;

    m_shortcut_key = WXK_CONTROL_R;

    return true;
}

std::string GLGizmoRotate3D::on_get_name() const
{    
    return L("Rotate");
}

void GLGizmoRotate3D::on_start_dragging(const GLCanvas3D::Selection& selection)
{
    if ((0 <= m_hover_id) && (m_hover_id < 3))
        m_gizmos[m_hover_id].start_dragging(selection);
}

void GLGizmoRotate3D::on_stop_dragging()
{
    if ((0 <= m_hover_id) && (m_hover_id < 3))
        m_gizmos[m_hover_id].stop_dragging();
}

void GLGizmoRotate3D::on_render(const GLCanvas3D::Selection& selection) const
{
    ::glClear(GL_DEPTH_BUFFER_BIT);

    if ((m_hover_id == -1) || (m_hover_id == 0))
        m_gizmos[X].render(selection);

    if ((m_hover_id == -1) || (m_hover_id == 1))
        m_gizmos[Y].render(selection);

    if ((m_hover_id == -1) || (m_hover_id == 2))
        m_gizmos[Z].render(selection);
}

#if ENABLE_IMGUI
void GLGizmoRotate3D::on_render_input_window(float x, float y, const GLCanvas3D::Selection& selection)
{
#if !DISABLE_MOVE_ROTATE_SCALE_GIZMOS_IMGUI
    Vec3d rotation(Geometry::rad2deg(m_gizmos[0].get_angle()), Geometry::rad2deg(m_gizmos[1].get_angle()), Geometry::rad2deg(m_gizmos[2].get_angle()));
    wxString label = _(L("Rotation (deg)"));

    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    m_imgui->set_next_window_bg_alpha(0.5f);
    m_imgui->begin(label, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    m_imgui->input_vec3("", rotation, 100.0f, "%.2f");
    m_imgui->end();
#endif // !DISABLE_MOVE_ROTATE_SCALE_GIZMOS_IMGUI
}
#endif // ENABLE_IMGUI

const float GLGizmoScale3D::Offset = 5.0f;

GLGizmoScale3D::GLGizmoScale3D(GLCanvas3D& parent)
    : GLGizmoBase(parent)
    , m_scale(Vec3d::Ones())
    , m_snap_step(0.05)
    , m_starting_scale(Vec3d::Ones())
{
}

bool GLGizmoScale3D::on_init()
{
    std::string path = resources_dir() + "/icons/overlay/";

    if (!m_textures[Off].load_from_file(path + "scale_off.png", false))
        return false;

    if (!m_textures[Hover].load_from_file(path + "scale_hover.png", false))
        return false;

    if (!m_textures[On].load_from_file(path + "scale_on.png", false))
        return false;

    for (int i = 0; i < 10; ++i)
    {
        m_grabbers.push_back(Grabber());
    }

    double half_pi = 0.5 * (double)PI;

    // x axis
    m_grabbers[0].angles(1) = half_pi;
    m_grabbers[1].angles(1) = half_pi;

    // y axis
    m_grabbers[2].angles(0) = half_pi;
    m_grabbers[3].angles(0) = half_pi;

    m_shortcut_key = WXK_CONTROL_S;

    return true;
}

std::string GLGizmoScale3D::on_get_name() const
{
    return L("Scale");
}

void GLGizmoScale3D::on_start_dragging(const GLCanvas3D::Selection& selection)
{
    if (m_hover_id != -1)
    {
        m_starting_drag_position = m_grabbers[m_hover_id].center;
        m_starting_box = selection.get_bounding_box();
    }
}

void GLGizmoScale3D::on_update(const UpdateData& data)
{
    if ((m_hover_id == 0) || (m_hover_id == 1))
        do_scale_x(data);
    else if ((m_hover_id == 2) || (m_hover_id == 3))
        do_scale_y(data);
    else if ((m_hover_id == 4) || (m_hover_id == 5))
        do_scale_z(data);
    else if (m_hover_id >= 6)
        do_scale_uniform(data);
}

void GLGizmoScale3D::on_render(const GLCanvas3D::Selection& selection) const
{
    bool single_instance = selection.is_single_full_instance();
    bool single_volume = selection.is_single_modifier() || selection.is_single_volume();
    bool single_selection = single_instance || single_volume;

    Vec3f scale = 100.0f * Vec3f::Ones();
#if ENABLE_MODELVOLUME_TRANSFORM
    if (single_instance)
        scale = 100.0f * selection.get_volume(*selection.get_volume_idxs().begin())->get_instance_scaling_factor().cast<float>();
    else if (single_volume)
        scale = 100.0f * selection.get_volume(*selection.get_volume_idxs().begin())->get_volume_scaling_factor().cast<float>();
#else
    Vec3f scale = single_instance ? 100.0f * selection.get_volume(*selection.get_volume_idxs().begin())->get_scaling_factor().cast<float>() : 100.0f * m_scale.cast<float>();
#endif // ENABLE_MODELVOLUME_TRANSFORM

    if ((single_selection && ((m_hover_id == 0) || (m_hover_id == 1))) || m_grabbers[0].dragging || m_grabbers[1].dragging)
        set_tooltip("X: " + format(scale(0), 4) + "%");
    else if (!m_grabbers[0].dragging && !m_grabbers[1].dragging && ((m_hover_id == 0) || (m_hover_id == 1)))
        set_tooltip("X");
    else if ((single_selection && ((m_hover_id == 2) || (m_hover_id == 3))) || m_grabbers[2].dragging || m_grabbers[3].dragging)
        set_tooltip("Y: " + format(scale(1), 4) + "%");
    else if (!m_grabbers[2].dragging && !m_grabbers[3].dragging && ((m_hover_id == 2) || (m_hover_id == 3)))
        set_tooltip("Y");
    else if ((single_selection && ((m_hover_id == 4) || (m_hover_id == 5))) || m_grabbers[4].dragging || m_grabbers[5].dragging)
        set_tooltip("Z: " + format(scale(2), 4) + "%");
    else if (!m_grabbers[4].dragging && !m_grabbers[5].dragging && ((m_hover_id == 4) || (m_hover_id == 5)))
        set_tooltip("Z");
    else if ((single_selection && ((m_hover_id == 6) || (m_hover_id == 7) || (m_hover_id == 8) || (m_hover_id == 9)))
        || m_grabbers[6].dragging || m_grabbers[7].dragging || m_grabbers[8].dragging || m_grabbers[9].dragging)
    {
        std::string tooltip = "X: " + format(scale(0), 4) + "%\n";
        tooltip += "Y: " + format(scale(1), 4) + "%\n";
        tooltip += "Z: " + format(scale(2), 4) + "%";
        set_tooltip(tooltip);
    }
    else if (!m_grabbers[6].dragging && !m_grabbers[7].dragging && !m_grabbers[8].dragging && !m_grabbers[9].dragging &&
        ((m_hover_id == 6) || (m_hover_id == 7) || (m_hover_id == 8) || (m_hover_id == 9)))
        set_tooltip("X/Y/Z");

    ::glClear(GL_DEPTH_BUFFER_BIT);
    ::glEnable(GL_DEPTH_TEST);

    BoundingBoxf3 box;
    Transform3d transform = Transform3d::Identity();
    Vec3d angles = Vec3d::Zero();
    Transform3d offsets_transform = Transform3d::Identity();

    Vec3d grabber_size = Vec3d::Zero();

    if (single_instance)
    {
        // calculate bounding box in instance local reference system
        const GLCanvas3D::Selection::IndicesList& idxs = selection.get_volume_idxs();
        for (unsigned int idx : idxs)
        {
            const GLVolume* vol = selection.get_volume(idx);
            box.merge(vol->bounding_box.transformed(vol->get_volume_transformation().get_matrix()));
        }

        // gets transform from first selected volume
        const GLVolume* v = selection.get_volume(*idxs.begin());
#if ENABLE_MODELVOLUME_TRANSFORM
        transform = v->get_instance_transformation().get_matrix();
        // gets angles from first selected volume
        angles = v->get_instance_rotation();
        // consider rotation+mirror only components of the transform for offsets
        offsets_transform = Geometry::assemble_transform(Vec3d::Zero(), angles, Vec3d::Ones(), v->get_instance_mirror());
        grabber_size = v->get_instance_transformation().get_matrix(true, true, false, true) * box.size();
#else
        transform = v->world_matrix().cast<double>();
        // gets angles from first selected volume
        angles = v->get_rotation();
        // consider rotation+mirror only components of the transform for offsets
        offsets_transform = Geometry::assemble_transform(Vec3d::Zero(), angles, Vec3d::Ones(), v->get_mirror());
#endif // ENABLE_MODELVOLUME_TRANSFORM
    }
    else if (single_volume)
    {
        const GLVolume* v = selection.get_volume(*selection.get_volume_idxs().begin());
        box = v->bounding_box;
#if ENABLE_MODELVOLUME_TRANSFORM
        transform = v->world_matrix();
        angles = Geometry::extract_euler_angles(transform);
        // consider rotation+mirror only components of the transform for offsets
        offsets_transform = Geometry::assemble_transform(Vec3d::Zero(), angles, Vec3d::Ones(), v->get_instance_mirror());
        grabber_size = v->get_volume_transformation().get_matrix(true, true, false, true) * box.size();
#else
        transform = v->world_matrix().cast<double>();
        angles = Geometry::extract_euler_angles(transform);
        // consider rotation+mirror only components of the transform for offsets
        offsets_transform = Geometry::assemble_transform(Vec3d::Zero(), angles, Vec3d::Ones(), v->get_mirror());
#endif // ENABLE_MODELVOLUME_TRANSFORM
    }
    else
    {
        box = selection.get_bounding_box();
        grabber_size = box.size();
    }

    m_box = box;

    const Vec3d& center = m_box.center();
    Vec3d offset_x = offsets_transform * Vec3d((double)Offset, 0.0, 0.0);
    Vec3d offset_y = offsets_transform * Vec3d(0.0, (double)Offset, 0.0);
    Vec3d offset_z = offsets_transform * Vec3d(0.0, 0.0, (double)Offset);

    // x axis
    m_grabbers[0].center = transform * Vec3d(m_box.min(0), center(1), center(2)) - offset_x;
    m_grabbers[1].center = transform * Vec3d(m_box.max(0), center(1), center(2)) + offset_x;
    ::memcpy((void*)m_grabbers[0].color, (const void*)&AXES_COLOR[0], 3 * sizeof(float));
    ::memcpy((void*)m_grabbers[1].color, (const void*)&AXES_COLOR[0], 3 * sizeof(float));

    // y axis
    m_grabbers[2].center = transform * Vec3d(center(0), m_box.min(1), center(2)) - offset_y;
    m_grabbers[3].center = transform * Vec3d(center(0), m_box.max(1), center(2)) + offset_y;
    ::memcpy((void*)m_grabbers[2].color, (const void*)&AXES_COLOR[1], 3 * sizeof(float));
    ::memcpy((void*)m_grabbers[3].color, (const void*)&AXES_COLOR[1], 3 * sizeof(float));

    // z axis
    m_grabbers[4].center = transform * Vec3d(center(0), center(1), m_box.min(2)) - offset_z;
    m_grabbers[5].center = transform * Vec3d(center(0), center(1), m_box.max(2)) + offset_z;
    ::memcpy((void*)m_grabbers[4].color, (const void*)&AXES_COLOR[2], 3 * sizeof(float));
    ::memcpy((void*)m_grabbers[5].color, (const void*)&AXES_COLOR[2], 3 * sizeof(float));

    // uniform
    m_grabbers[6].center = transform * Vec3d(m_box.min(0), m_box.min(1), center(2)) - offset_x - offset_y;
    m_grabbers[7].center = transform * Vec3d(m_box.max(0), m_box.min(1), center(2)) + offset_x - offset_y;
    m_grabbers[8].center = transform * Vec3d(m_box.max(0), m_box.max(1), center(2)) + offset_x + offset_y;
    m_grabbers[9].center = transform * Vec3d(m_box.min(0), m_box.max(1), center(2)) - offset_x + offset_y;
    for (int i = 6; i < 10; ++i)
    {
        ::memcpy((void*)m_grabbers[i].color, (const void*)m_highlight_color, 3 * sizeof(float));
    }

    // sets grabbers orientation
    for (int i = 0; i < 10; ++i)
    {
        m_grabbers[i].angles = angles;
    }

    ::glLineWidth((m_hover_id != -1) ? 2.0f : 1.5f);

    float grabber_max_size = (float)std::max(grabber_size(0), std::max(grabber_size(1), grabber_size(2)));

    if (m_hover_id == -1)
    {
        // draw connections
        if (m_grabbers[0].enabled && m_grabbers[1].enabled)
        {
            ::glColor3fv(m_grabbers[0].color);
            render_grabbers_connection(0, 1);
        }
        if (m_grabbers[2].enabled && m_grabbers[3].enabled)
        {
            ::glColor3fv(m_grabbers[2].color);
            render_grabbers_connection(2, 3);
        }
        if (m_grabbers[4].enabled && m_grabbers[5].enabled)
        {
            ::glColor3fv(m_grabbers[4].color);
            render_grabbers_connection(4, 5);
        }
        ::glColor3fv(m_base_color);
        render_grabbers_connection(6, 7);
        render_grabbers_connection(7, 8);
        render_grabbers_connection(8, 9);
        render_grabbers_connection(9, 6);
        // draw grabbers
        render_grabbers(grabber_max_size);
    }
    else if ((m_hover_id == 0) || (m_hover_id == 1))
    {
        // draw connection
        ::glColor3fv(m_grabbers[0].color);
        render_grabbers_connection(0, 1);
        // draw grabbers
        m_grabbers[0].render(true, grabber_max_size);
        m_grabbers[1].render(true, grabber_max_size);
    }
    else if ((m_hover_id == 2) || (m_hover_id == 3))
    {
        // draw connection
        ::glColor3fv(m_grabbers[2].color);
        render_grabbers_connection(2, 3);
        // draw grabbers
        m_grabbers[2].render(true, grabber_max_size);
        m_grabbers[3].render(true, grabber_max_size);
    }
    else if ((m_hover_id == 4) || (m_hover_id == 5))
    {
        // draw connection
        ::glColor3fv(m_grabbers[4].color);
        render_grabbers_connection(4, 5);
        // draw grabbers
        m_grabbers[4].render(true, grabber_max_size);
        m_grabbers[5].render(true, grabber_max_size);
    }
    else if (m_hover_id >= 6)
    {
        // draw connection
        ::glColor3fv(m_drag_color);
        render_grabbers_connection(6, 7);
        render_grabbers_connection(7, 8);
        render_grabbers_connection(8, 9);
        render_grabbers_connection(9, 6);
        // draw grabbers
        for (int i = 6; i < 10; ++i)
        {
            m_grabbers[i].render(true, grabber_max_size);
        }
    }
}

void GLGizmoScale3D::on_render_for_picking(const GLCanvas3D::Selection& selection) const
{
    ::glDisable(GL_DEPTH_TEST);

    render_grabbers_for_picking(selection.get_bounding_box());
}

#if ENABLE_IMGUI
void GLGizmoScale3D::on_render_input_window(float x, float y, const GLCanvas3D::Selection& selection)
{
#if !DISABLE_MOVE_ROTATE_SCALE_GIZMOS_IMGUI
    bool single_instance = selection.is_single_full_instance();
    wxString label = _(L("Scale (%)"));

    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    m_imgui->set_next_window_bg_alpha(0.5f);
    m_imgui->begin(label, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    m_imgui->input_vec3("", m_scale * 100.f, 100.0f, "%.2f");
    m_imgui->end();
#endif // !DISABLE_MOVE_ROTATE_SCALE_GIZMOS_IMGUI
}
#endif // ENABLE_IMGUI

void GLGizmoScale3D::render_grabbers_connection(unsigned int id_1, unsigned int id_2) const
{
    unsigned int grabbers_count = (unsigned int)m_grabbers.size();
    if ((id_1 < grabbers_count) && (id_2 < grabbers_count))
    {
        ::glBegin(GL_LINES);
        ::glVertex3dv(m_grabbers[id_1].center.data());
        ::glVertex3dv(m_grabbers[id_2].center.data());
        ::glEnd();
    }
}

void GLGizmoScale3D::do_scale_x(const UpdateData& data)
{
    double ratio = calc_ratio(data);
    if (ratio > 0.0)
        m_scale(0) = m_starting_scale(0) * ratio;
}

void GLGizmoScale3D::do_scale_y(const UpdateData& data)
{
    double ratio = calc_ratio(data);
    if (ratio > 0.0)
        m_scale(1) = m_starting_scale(1) * ratio;
}

void GLGizmoScale3D::do_scale_z(const UpdateData& data)
{
    double ratio = calc_ratio(data);
    if (ratio > 0.0)
        m_scale(2) = m_starting_scale(2) * ratio;
}

void GLGizmoScale3D::do_scale_uniform(const UpdateData& data)
{
    double ratio = calc_ratio(data);
    if (ratio > 0.0)
        m_scale = m_starting_scale * ratio;
}

double GLGizmoScale3D::calc_ratio(const UpdateData& data) const
{
    double ratio = 0.0;

    // vector from the center to the starting position
    Vec3d starting_vec = m_starting_drag_position - m_starting_box.center();
    double len_starting_vec = starting_vec.norm();
    if (len_starting_vec != 0.0)
    {
        Vec3d mouse_dir = data.mouse_ray.unit_vector();
        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
        // in our case plane normal and ray direction are the same (orthogonal view)
        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
        Vec3d inters = data.mouse_ray.a + (m_starting_drag_position - data.mouse_ray.a).dot(mouse_dir) / mouse_dir.squaredNorm() * mouse_dir;
        // vector from the starting position to the found intersection
        Vec3d inters_vec = inters - m_starting_drag_position;

        // finds projection of the vector along the staring direction
        double proj = inters_vec.dot(starting_vec.normalized());

        ratio = (len_starting_vec + proj) / len_starting_vec;
    }

    if (data.shift_down)
        ratio = m_snap_step * (double)std::round(ratio / m_snap_step);

    return ratio;
}

const double GLGizmoMove3D::Offset = 10.0;

GLGizmoMove3D::GLGizmoMove3D(GLCanvas3D& parent)
    : GLGizmoBase(parent)
    , m_displacement(Vec3d::Zero())
    , m_snap_step(1.0)
    , m_starting_drag_position(Vec3d::Zero())
    , m_starting_box_center(Vec3d::Zero())
    , m_starting_box_bottom_center(Vec3d::Zero())
    , m_quadric(nullptr)
{
    m_quadric = ::gluNewQuadric();
    if (m_quadric != nullptr)
        ::gluQuadricDrawStyle(m_quadric, GLU_FILL);
}

GLGizmoMove3D::~GLGizmoMove3D()
{
    if (m_quadric != nullptr)
        ::gluDeleteQuadric(m_quadric);
}

bool GLGizmoMove3D::on_init()
{
    std::string path = resources_dir() + "/icons/overlay/";

    if (!m_textures[Off].load_from_file(path + "move_off.png", false))
        return false;

    if (!m_textures[Hover].load_from_file(path + "move_hover.png", false))
        return false;

    if (!m_textures[On].load_from_file(path + "move_on.png", false))
        return false;

    for (int i = 0; i < 3; ++i)
    {
        m_grabbers.push_back(Grabber());
    }

    m_shortcut_key = WXK_CONTROL_M;

    return true;
}

std::string GLGizmoMove3D::on_get_name() const
{
    return L("Move");
}

void GLGizmoMove3D::on_start_dragging(const GLCanvas3D::Selection& selection)
{
    if (m_hover_id != -1)
    {
        m_displacement = Vec3d::Zero();
        const BoundingBoxf3& box = selection.get_bounding_box();
        m_starting_drag_position = m_grabbers[m_hover_id].center;
        m_starting_box_center = box.center();
        m_starting_box_bottom_center = box.center();
        m_starting_box_bottom_center(2) = box.min(2);
    }
}

void GLGizmoMove3D::on_stop_dragging()
{
    m_displacement = Vec3d::Zero();
}

void GLGizmoMove3D::on_update(const UpdateData& data)
{
    if (m_hover_id == 0)
        m_displacement(0) = calc_projection(data);
    else if (m_hover_id == 1)
        m_displacement(1) = calc_projection(data);
    else if (m_hover_id == 2)
        m_displacement(2) = calc_projection(data);
}

void GLGizmoMove3D::on_render(const GLCanvas3D::Selection& selection) const
{
    bool show_position = selection.is_single_full_instance();
    const Vec3d& position = selection.get_bounding_box().center();

    if ((show_position && (m_hover_id == 0)) || m_grabbers[0].dragging)
        set_tooltip("X: " + format(show_position ? position(0) : m_displacement(0), 2));
    else if (!m_grabbers[0].dragging && (m_hover_id == 0))
        set_tooltip("X");
    else if ((show_position && (m_hover_id == 1)) || m_grabbers[1].dragging)
        set_tooltip("Y: " + format(show_position ? position(1) : m_displacement(1), 2));
    else if (!m_grabbers[1].dragging && (m_hover_id == 1))
        set_tooltip("Y");
    else if ((show_position && (m_hover_id == 2)) || m_grabbers[2].dragging)
        set_tooltip("Z: " + format(show_position ? position(2) : m_displacement(2), 2));
    else if (!m_grabbers[2].dragging && (m_hover_id == 2))
        set_tooltip("Z");

    ::glClear(GL_DEPTH_BUFFER_BIT);
    ::glEnable(GL_DEPTH_TEST);

    const BoundingBoxf3& box = selection.get_bounding_box();
    const Vec3d& center = box.center();

    // x axis
    m_grabbers[0].center = Vec3d(box.max(0) + Offset, center(1), center(2));
    ::memcpy((void*)m_grabbers[0].color, (const void*)&AXES_COLOR[0], 3 * sizeof(float));

    // y axis
    m_grabbers[1].center = Vec3d(center(0), box.max(1) + Offset, center(2));
    ::memcpy((void*)m_grabbers[1].color, (const void*)&AXES_COLOR[1], 3 * sizeof(float));

    // z axis
    m_grabbers[2].center = Vec3d(center(0), center(1), box.max(2) + Offset);
    ::memcpy((void*)m_grabbers[2].color, (const void*)&AXES_COLOR[2], 3 * sizeof(float));

    ::glLineWidth((m_hover_id != -1) ? 2.0f : 1.5f);

    if (m_hover_id == -1)
    {
        // draw axes
        for (unsigned int i = 0; i < 3; ++i)
        {
            if (m_grabbers[i].enabled)
            {
                ::glColor3fv(AXES_COLOR[i]);
                ::glBegin(GL_LINES);
                ::glVertex3dv(center.data());
                ::glVertex3dv(m_grabbers[i].center.data());
                ::glEnd();
            }
        }

        // draw grabbers
        render_grabbers(box);
        for (unsigned int i = 0; i < 3; ++i)
        {
            if (m_grabbers[i].enabled)
                render_grabber_extension((Axis)i, box, false);
        }
    }
    else
    {
        // draw axis
        ::glColor3fv(AXES_COLOR[m_hover_id]);
        ::glBegin(GL_LINES);
        ::glVertex3dv(center.data());
        ::glVertex3dv(m_grabbers[m_hover_id].center.data());
        ::glEnd();

        // draw grabber
        m_grabbers[m_hover_id].render(true, box.max_size());
        render_grabber_extension((Axis)m_hover_id, box, false);
    }
}

void GLGizmoMove3D::on_render_for_picking(const GLCanvas3D::Selection& selection) const
{
    ::glDisable(GL_DEPTH_TEST);

    const BoundingBoxf3& box = selection.get_bounding_box();
    render_grabbers_for_picking(box);
    render_grabber_extension(X, box, true);
    render_grabber_extension(Y, box, true);
    render_grabber_extension(Z, box, true);
}

#if ENABLE_IMGUI
void GLGizmoMove3D::on_render_input_window(float x, float y, const GLCanvas3D::Selection& selection)
{
#if !DISABLE_MOVE_ROTATE_SCALE_GIZMOS_IMGUI
    bool show_position = selection.is_single_full_instance();
    const Vec3d& position = selection.get_bounding_box().center();

    Vec3d displacement = show_position ? position : m_displacement;
    wxString label = show_position ? _(L("Position (mm)")) : _(L("Displacement (mm)"));

    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    m_imgui->set_next_window_bg_alpha(0.5f);
    m_imgui->begin(label, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    m_imgui->input_vec3("", displacement, 100.0f, "%.2f");

    m_imgui->end();
#endif // !DISABLE_MOVE_ROTATE_SCALE_GIZMOS_IMGUI
}
#endif // ENABLE_IMGUI

double GLGizmoMove3D::calc_projection(const UpdateData& data) const
{
    double projection = 0.0;

    Vec3d starting_vec = m_starting_drag_position - m_starting_box_center;
    double len_starting_vec = starting_vec.norm();
    if (len_starting_vec != 0.0)
    {
        Vec3d mouse_dir = data.mouse_ray.unit_vector();
        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
        // in our case plane normal and ray direction are the same (orthogonal view)
        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
        Vec3d inters = data.mouse_ray.a + (m_starting_drag_position - data.mouse_ray.a).dot(mouse_dir) / mouse_dir.squaredNorm() * mouse_dir;
        // vector from the starting position to the found intersection
        Vec3d inters_vec = inters - m_starting_drag_position;

        // finds projection of the vector along the staring direction
        projection = inters_vec.dot(starting_vec.normalized());
    }

    if (data.shift_down)
        projection = m_snap_step * (double)std::round(projection / m_snap_step);

    return projection;
}

void GLGizmoMove3D::render_grabber_extension(Axis axis, const BoundingBoxf3& box, bool picking) const
{
    if (m_quadric == nullptr)
        return;

    double size = m_dragging ? (double)m_grabbers[axis].get_dragging_half_size((float)box.max_size()) : (double)m_grabbers[axis].get_half_size((float)box.max_size());

    float color[3];
    ::memcpy((void*)color, (const void*)m_grabbers[axis].color, 3 * sizeof(float));
    if (!picking && (m_hover_id != -1))
    {
        color[0] = 1.0f - color[0];
        color[1] = 1.0f - color[1];
        color[2] = 1.0f - color[2];
    }

    if (!picking)
        ::glEnable(GL_LIGHTING);

    ::glColor3fv(color);
    ::glPushMatrix();
    ::glTranslated(m_grabbers[axis].center(0), m_grabbers[axis].center(1), m_grabbers[axis].center(2));
    if (axis == X)
        ::glRotated(90.0, 0.0, 1.0, 0.0);
    else if (axis == Y)
        ::glRotated(-90.0, 1.0, 0.0, 0.0);

    ::glTranslated(0.0, 0.0, 2.0 * size);
    ::gluQuadricOrientation(m_quadric, GLU_OUTSIDE);
    ::gluCylinder(m_quadric, 0.75 * size, 0.0, 3.0 * size, 36, 1);
    ::gluQuadricOrientation(m_quadric, GLU_INSIDE);
    ::gluDisk(m_quadric, 0.0, 0.75 * size, 36, 1);
    ::glPopMatrix();

    if (!picking)
        ::glDisable(GL_LIGHTING);
}

GLGizmoFlatten::GLGizmoFlatten(GLCanvas3D& parent)
    : GLGizmoBase(parent)
    , m_normal(Vec3d::Zero())
    , m_starting_center(Vec3d::Zero())
{
}

bool GLGizmoFlatten::on_init()
{
    std::string path = resources_dir() + "/icons/overlay/";

    if (!m_textures[Off].load_from_file(path + "layflat_off.png", false))
        return false;

    if (!m_textures[Hover].load_from_file(path + "layflat_hover.png", false))
        return false;

    if (!m_textures[On].load_from_file(path + "layflat_on.png", false))
        return false;

    m_shortcut_key = WXK_CONTROL_F;

    return true;
}

std::string GLGizmoFlatten::on_get_name() const
{
    return L("Place on face");
}

bool GLGizmoFlatten::on_is_activable(const GLCanvas3D::Selection& selection) const
{
    return selection.is_single_full_instance();
}

void GLGizmoFlatten::on_start_dragging(const GLCanvas3D::Selection& selection)
{
    if (m_hover_id != -1)
    {
        m_normal = m_planes[m_hover_id].normal;
        m_starting_center = selection.get_bounding_box().center();
    }
}

void GLGizmoFlatten::on_render(const GLCanvas3D::Selection& selection) const
{
    ::glClear(GL_DEPTH_BUFFER_BIT);

    ::glEnable(GL_DEPTH_TEST);
    ::glEnable(GL_BLEND);

    if (selection.is_single_full_instance())
    {
        const Transform3d& m = selection.get_volume(*selection.get_volume_idxs().begin())->get_instance_transformation().get_matrix();
        ::glPushMatrix();
        ::glMultMatrixd(m.data());
        for (int i = 0; i < (int)m_planes.size(); ++i)
        {
            if (i == m_hover_id)
                ::glColor4f(0.9f, 0.9f, 0.9f, 0.75f);
            else
                ::glColor4f(0.9f, 0.9f, 0.9f, 0.5f);

            ::glBegin(GL_POLYGON);
            for (const Vec3d& vertex : m_planes[i].vertices)
            {
                ::glVertex3dv(vertex.data());
            }
            ::glEnd();
        }
        ::glPopMatrix();
    }

    ::glEnable(GL_CULL_FACE);
    ::glDisable(GL_BLEND);
}

void GLGizmoFlatten::on_render_for_picking(const GLCanvas3D::Selection& selection) const
{
    ::glDisable(GL_DEPTH_TEST);
    ::glDisable(GL_BLEND);

    if (selection.is_single_full_instance())
    {
        const Transform3d& m = selection.get_volume(*selection.get_volume_idxs().begin())->get_instance_transformation().get_matrix();
        ::glPushMatrix();
        ::glMultMatrixd(m.data());
        for (int i = 0; i < (int)m_planes.size(); ++i)
        {
            ::glColor3f(1.0f, 1.0f, picking_color_component(i));
            ::glBegin(GL_POLYGON);
            for (const Vec3d& vertex : m_planes[i].vertices)
            {
                ::glVertex3dv(vertex.data());
            }
            ::glEnd();
        }
        ::glPopMatrix();
    }

    ::glEnable(GL_CULL_FACE);
}

void GLGizmoFlatten::set_flattening_data(const ModelObject* model_object)
{
    m_starting_center = Vec3d::Zero();
    bool object_changed = m_model_object != model_object;
    m_model_object = model_object;

    if (object_changed && is_plane_update_necessary())
        update_planes();
}

void GLGizmoFlatten::update_planes()
{
    TriangleMesh ch;
    for (const ModelVolume* vol : m_model_object->volumes)
#if ENABLE_MODELVOLUME_TRANSFORM
    {
        if (vol->type() != ModelVolume::Type::MODEL_PART)
            continue;
        TriangleMesh vol_ch = vol->get_convex_hull();
        vol_ch.transform(vol->get_matrix());
        ch.merge(vol_ch);
    }
#else
        ch.merge(vol->get_convex_hull());
#endif // ENABLE_MODELVOLUME_TRANSFORM

    ch = ch.convex_hull_3d();

    const Vec3d& bb_size = ch.bounding_box().size();
    double min_bb_face_area = std::min(bb_size(0) * bb_size(1), std::min(bb_size(0) * bb_size(2), bb_size(1) * bb_size(2)));

    m_planes.clear();

    // Now we'll go through all the facets and append Points of facets sharing the same normal:
    const int num_of_facets = ch.stl.stats.number_of_facets;
    std::vector<int>  facet_queue(num_of_facets, 0);
    std::vector<bool> facet_visited(num_of_facets, false);
    int               facet_queue_cnt = 0;
    const stl_normal* normal_ptr = nullptr;
    while (1) {
        // Find next unvisited triangle:
        int facet_idx = 0;
        for (; facet_idx < num_of_facets; ++ facet_idx)
            if (!facet_visited[facet_idx]) {
                facet_queue[facet_queue_cnt ++] = facet_idx;
                facet_visited[facet_idx] = true;
                normal_ptr = &ch.stl.facet_start[facet_idx].normal;
                m_planes.emplace_back();
                break;
            }
        if (facet_idx == num_of_facets)
            break; // Everything was visited already

        while (facet_queue_cnt > 0) {
            int facet_idx = facet_queue[-- facet_queue_cnt];
            const stl_normal& this_normal = ch.stl.facet_start[facet_idx].normal;
            if (std::abs(this_normal(0) - (*normal_ptr)(0)) < 0.001 && std::abs(this_normal(1) - (*normal_ptr)(1)) < 0.001 && std::abs(this_normal(2) - (*normal_ptr)(2)) < 0.001) {
                stl_vertex* first_vertex = ch.stl.facet_start[facet_idx].vertex;
                for (int j=0; j<3; ++j)
                    m_planes.back().vertices.emplace_back((double)first_vertex[j](0), (double)first_vertex[j](1), (double)first_vertex[j](2));

                facet_visited[facet_idx] = true;
                for (int j = 0; j < 3; ++ j) {
                    int neighbor_idx = ch.stl.neighbors_start[facet_idx].neighbor[j];
                    if (! facet_visited[neighbor_idx])
                        facet_queue[facet_queue_cnt ++] = neighbor_idx;
                }
            }
        }
        m_planes.back().normal = Vec3d((double)(*normal_ptr)(0), (double)(*normal_ptr)(1), (double)(*normal_ptr)(2));

        // if this is a just a very small triangle, remove it to speed up further calculations (it would be rejected anyway):
        if (m_planes.back().vertices.size() == 3 &&
            ((m_planes.back().vertices[0] - m_planes.back().vertices[1]).norm() < 1.0
            || (m_planes.back().vertices[0] - m_planes.back().vertices[2]).norm() < 1.0
            || (m_planes.back().vertices[1] - m_planes.back().vertices[2]).norm() < 1.0))
            m_planes.pop_back();
    }

    const float minimal_area = 0.01f * (float)min_bb_face_area;

    // Now we'll go through all the polygons, transform the points into xy plane to process them:
    for (unsigned int polygon_id=0; polygon_id < m_planes.size(); ++polygon_id) {
        Pointf3s& polygon = m_planes[polygon_id].vertices;
        const Vec3d& normal = m_planes[polygon_id].normal;

        // We are going to rotate about z and y to flatten the plane
        Eigen::Quaterniond q;
        Transform3d m = Transform3d::Identity();
        m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(normal, Vec3d::UnitZ()).toRotationMatrix();
        polygon = transform(polygon, m);

        polygon = Slic3r::Geometry::convex_hull(polygon); // To remove the inner points

        // We will calculate area of the polygons and discard ones that are too small
        // The limit is more forgiving in case the normal is in the direction of the coordinate axes
        float area_threshold = (std::abs(normal(0)) > 0.999f || std::abs(normal(1)) > 0.999f || std::abs(normal(2)) > 0.999f) ? minimal_area : 10.0f * minimal_area;
        float& area = m_planes[polygon_id].area;
        area = 0.f;
        for (unsigned int i = 0; i < polygon.size(); i++) // Shoelace formula
            area += polygon[i](0)*polygon[i + 1 < polygon.size() ? i + 1 : 0](1) - polygon[i + 1 < polygon.size() ? i + 1 : 0](0)*polygon[i](1);
        area = 0.5f * std::abs(area);
        if (area < area_threshold) {
            m_planes.erase(m_planes.begin()+(polygon_id--));
            continue;
        }

        // We check the inner angles and discard polygons with angles smaller than the following threshold
        const double angle_threshold = ::cos(10.0 * (double)PI / 180.0);
        bool discard = false;

        for (unsigned int i = 0; i < polygon.size(); ++i)
        {
            const Vec3d& prec = polygon[(i == 0) ? polygon.size() - 1 : i - 1];
            const Vec3d& curr = polygon[i];
            const Vec3d& next = polygon[(i == polygon.size() - 1) ? 0 : i + 1];

            if ((prec - curr).normalized().dot((next - curr).normalized()) > angle_threshold)
            {
                discard = true;
                break;
            }
        }

        if (discard)
        {
            m_planes.erase(m_planes.begin() + (polygon_id--));
            continue;
        }

        // We will shrink the polygon a little bit so it does not touch the object edges:
        Vec3d centroid = std::accumulate(polygon.begin(), polygon.end(), Vec3d(0.0, 0.0, 0.0));
        centroid /= (double)polygon.size();
        for (auto& vertex : polygon)
            vertex = 0.9f*vertex + 0.1f*centroid;

        // Polygon is now simple and convex, we'll round the corners to make them look nicer.
        // The algorithm takes a vertex, calculates middles of respective sides and moves the vertex
        // towards their average (controlled by 'aggressivity'). This is repeated k times.
        // In next iterations, the neighbours are not always taken at the middle (to increase the
        // rounding effect at the corners, where we need it most).
        const unsigned int k = 10; // number of iterations
        const float aggressivity = 0.2f;  // agressivity
        const unsigned int N = polygon.size();
        std::vector<std::pair<unsigned int, unsigned int>> neighbours;
        if (k != 0) {
            Pointf3s points_out(2*k*N); // vector long enough to store the future vertices
            for (unsigned int j=0; j<N; ++j) {
                points_out[j*2*k] = polygon[j];
                neighbours.push_back(std::make_pair((int)(j*2*k-k) < 0 ? (N-1)*2*k+k : j*2*k-k, j*2*k+k));
            }

            for (unsigned int i=0; i<k; ++i) {
                // Calculate middle of each edge so that neighbours points to something useful:
                for (unsigned int j=0; j<N; ++j)
                    if (i==0)
                        points_out[j*2*k+k] = 0.5f * (points_out[j*2*k] + points_out[j==N-1 ? 0 : (j+1)*2*k]);
                    else {
                        float r = 0.2+0.3/(k-1)*i; // the neighbours are not always taken in the middle
                        points_out[neighbours[j].first] = r*points_out[j*2*k] + (1-r) * points_out[neighbours[j].first-1];
                        points_out[neighbours[j].second] = r*points_out[j*2*k] + (1-r) * points_out[neighbours[j].second+1];
                    }
                // Now we have a triangle and valid neighbours, we can do an iteration:
                for (unsigned int j=0; j<N; ++j)
                    points_out[2*k*j] = (1-aggressivity) * points_out[2*k*j] +
                                        aggressivity*0.5f*(points_out[neighbours[j].first] + points_out[neighbours[j].second]);

                for (auto& n : neighbours) {
                    ++n.first;
                    --n.second;
                }
            }
            polygon = points_out; // replace the coarse polygon with the smooth one that we just created
        }

        // Transform back to 3D;
        for (auto& b : polygon) {
            b(2) += 0.1f; // raise a bit above the object surface to avoid flickering
        }

        m = m.inverse();
        polygon = transform(polygon, m);
    }

    // We'll sort the planes by area and only keep the 254 largest ones (because of the picking pass limitations):
    std::sort(m_planes.rbegin(), m_planes.rend(), [](const PlaneData& a, const PlaneData& b) { return a.area < b.area; });
    m_planes.resize(std::min((int)m_planes.size(), 254));

    // Planes are finished - let's save what we calculated it from:
    m_volumes_matrices.clear();
    for (const ModelVolume* vol : m_model_object->volumes)
        m_volumes_matrices.push_back(vol->get_matrix());
}

// Check if the bounding boxes of each volume's convex hull is the same as before
// and that scaling and rotation has not changed. In that case we don't have to recalculate it.
bool GLGizmoFlatten::is_plane_update_necessary() const
{
    if (m_state != On || !m_model_object || m_model_object->instances.empty())
        return false;

    if (m_model_object->volumes.size() != m_volumes_matrices.size())
        return true;

    for (unsigned int i=0; i < m_model_object->volumes.size(); ++i)
        if (! m_model_object->volumes[i]->get_matrix().isApprox(m_volumes_matrices[i]))
            return true;

    return false;
}

Vec3d GLGizmoFlatten::get_flattening_normal() const
{
    Vec3d out = m_normal;
    m_normal = Vec3d::Zero();
    m_starting_center = Vec3d::Zero();
    return out;
}

GLGizmoSlaSupports::GLGizmoSlaSupports(GLCanvas3D& parent)
#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    : GLGizmoBase(parent), m_starting_center(Vec3d::Zero()), m_quadric(nullptr)
#else
    : GLGizmoBase(parent), m_starting_center(Vec3d::Zero())
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD
{
#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    m_quadric = ::gluNewQuadric();
    if (m_quadric != nullptr)
        // using GLU_FILL does not work when the instance's transformation
        // contains mirroring (normals are reverted)
        ::gluQuadricDrawStyle(m_quadric, GLU_SILHOUETTE);
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD
}

#if ENABLE_SLA_SUPPORT_GIZMO_MOD
GLGizmoSlaSupports::~GLGizmoSlaSupports()
{
    if (m_quadric != nullptr)
        ::gluDeleteQuadric(m_quadric);
}
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD

bool GLGizmoSlaSupports::on_init()
{
    std::string path = resources_dir() + "/icons/overlay/";

    if (!m_textures[Off].load_from_file(path + "sla_support_points_off.png", false))
        return false;

    if (!m_textures[Hover].load_from_file(path + "sla_support_points_hover.png", false))
        return false;

    if (!m_textures[On].load_from_file(path + "sla_support_points_on.png", false))
        return false;

    m_shortcut_key = WXK_CONTROL_L;

    return true;
}

#if ENABLE_SLA_SUPPORT_GIZMO_MOD
void GLGizmoSlaSupports::set_sla_support_data(ModelObject* model_object, const GLCanvas3D::Selection& selection)
{
    m_starting_center = Vec3d::Zero();
    m_old_model_object = m_model_object;
    m_model_object = model_object;
    if (selection.is_empty())
        m_old_instance_id = -1;

    if ((model_object != nullptr) && selection.is_from_single_instance())
    {
        if (is_mesh_update_necessary())
            update_mesh();
    }
}
#else
void GLGizmoSlaSupports::set_model_object_ptr(ModelObject* model_object)
{
    if (model_object != nullptr) {
        m_starting_center = Vec3d::Zero();
        m_model_object = model_object;

        int selected_instance = m_parent.get_selection().get_instance_idx();
        assert(selected_instance < (int)model_object->instances.size());

        m_instance_matrix = model_object->instances[selected_instance]->get_matrix();
        if (is_mesh_update_necessary())
            update_mesh();
    }
}
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD

void GLGizmoSlaSupports::on_render(const GLCanvas3D::Selection& selection) const
{
    ::glEnable(GL_BLEND);
    ::glEnable(GL_DEPTH_TEST);

#if !ENABLE_SLA_SUPPORT_GIZMO_MOD
    // the dragged_offset is a vector measuring where was the object moved
    // with the gizmo being on. This is reset in set_model_object_ptr and
    // does not work correctly when there are multiple copies.
    
    if (m_starting_center == Vec3d::Zero())
        m_starting_center = selection.get_bounding_box().center();
    Vec3d dragged_offset = selection.get_bounding_box().center() - m_starting_center;
#endif // !ENABLE_SLA_SUPPORT_GIZMO_MOD


    for (auto& g : m_grabbers) {
        g.color[0] = 1.f;
        g.color[1] = 0.f;
        g.color[2] = 0.f;
    }

#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    render_grabbers(selection, false);
#else
    //::glTranslatef((GLfloat)dragged_offset(0), (GLfloat)dragged_offset(1), (GLfloat)dragged_offset(2));
    render_grabbers(false);
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD

#if !ENABLE_IMGUI
    render_tooltip_texture();
#endif // not ENABLE_IMGUI
    ::glDisable(GL_BLEND);
}


void GLGizmoSlaSupports::on_render_for_picking(const GLCanvas3D::Selection& selection) const
{
    ::glEnable(GL_DEPTH_TEST);
    for (unsigned int i=0; i<m_grabbers.size(); ++i) {
        m_grabbers[i].color[0] = 1.0f;
        m_grabbers[i].color[1] = 1.0f;
        m_grabbers[i].color[2] = picking_color_component(i);
    }
#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    render_grabbers(selection, true);
#else
    render_grabbers(true);
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD
}

#if ENABLE_SLA_SUPPORT_GIZMO_MOD
void GLGizmoSlaSupports::render_grabbers(const GLCanvas3D::Selection& selection, bool picking) const
{
    if (m_quadric == nullptr)
        return;

    if (!selection.is_from_single_instance())
        return;

    const GLVolume* v = selection.get_volume(*selection.get_volume_idxs().begin());
    double z_shift = v->get_sla_shift_z();

    ::glPushMatrix();
    ::glTranslated(0.0, 0.0, z_shift);

    const Transform3d& m = selection.get_volume(*selection.get_volume_idxs().begin())->get_instance_transformation().get_matrix();
    ::glMultMatrixd(m.data());

    if (!picking)
        ::glEnable(GL_LIGHTING);

    float render_color[3];
    for (int i = 0; i < (int)m_grabbers.size(); ++i)
    {
        // first precalculate the grabber position in world coordinates, so that the grabber
        // is not scaled with the object (as it would be if rendered with current gl matrix).
        Eigen::Matrix<GLfloat, 4, 4> glmatrix;
        glGetFloatv (GL_MODELVIEW_MATRIX, glmatrix.data());
        Eigen::Matrix<float, 4, 1> grabber_pos;
        for (int j=0; j<3; ++j)
            grabber_pos(j) = m_grabbers[i].center(j);
        grabber_pos[3] = 1.f;
        Eigen::Matrix<float, 4, 1> grabber_world_position = glmatrix * grabber_pos;

        if (!picking && (m_hover_id == i))
        {
            render_color[0] = 1.0f - m_grabbers[i].color[0];
            render_color[1] = 1.0f - m_grabbers[i].color[1];
            render_color[2] = 1.0f - m_grabbers[i].color[2];
        }
        else
            ::memcpy((void*)render_color, (const void*)m_grabbers[i].color, 3 * sizeof(float));

        ::glColor3fv(render_color);
        ::glPushMatrix();
        ::glLoadIdentity();
        ::glTranslated(grabber_world_position(0), grabber_world_position(1), grabber_world_position(2) + z_shift);
        ::gluQuadricDrawStyle(m_quadric, GLU_SILHOUETTE);
        ::gluSphere(m_quadric, 0.75, 64, 36);
        ::glPopMatrix();
    }

    if (!picking)
        ::glDisable(GL_LIGHTING);

    ::glPopMatrix();
}
#else
void GLGizmoSlaSupports::render_grabbers(bool picking) const
{
	if (m_parent.get_selection().is_empty())
		return;

    float z_shift = m_parent.get_selection().get_volume(0)->get_sla_shift_z();
    ::glTranslatef((GLfloat)0, (GLfloat)0, (GLfloat)z_shift);

    int selected_instance = m_parent.get_selection().get_instance_idx();
    assert(selected_instance < (int)m_model_object->instances.size());

    float render_color_inactive[3] = { 0.5f, 0.5f, 0.5f };

    for (const ModelInstance* inst : m_model_object->instances) {
		bool active = inst == m_model_object->instances[selected_instance];
        if (picking && ! active)
            continue;
        for (int i = 0; i < (int)m_grabbers.size(); ++i)
        {
            if (!m_grabbers[i].enabled)
                continue;

            float render_color[3];
            if (! picking && active && m_hover_id == i) {
                render_color[0] = 1.0f - m_grabbers[i].color[0];
                render_color[1] = 1.0f - m_grabbers[i].color[1];
                render_color[2] = 1.0f - m_grabbers[i].color[2];
            }
            else
                ::memcpy((void*)render_color, active ? (const void*)m_grabbers[i].color : (const void*)render_color_inactive, 3 * sizeof(float));
            if (!picking)
                ::glEnable(GL_LIGHTING);
            ::glColor3f((GLfloat)render_color[0], (GLfloat)render_color[1], (GLfloat)render_color[2]);
            ::glPushMatrix();
            Vec3d center = inst->get_matrix() * m_grabbers[i].center;
            ::glTranslatef((GLfloat)center(0), (GLfloat)center(1), (GLfloat)center(2));
            GLUquadricObj *quadric;
            quadric = ::gluNewQuadric();
            ::gluQuadricDrawStyle(quadric, GLU_FILL );
            ::gluSphere( quadric , 0.75f, 64 , 32 );
            ::gluDeleteQuadric(quadric);
            ::glPopMatrix();
            if (!picking)
                ::glDisable(GL_LIGHTING);
        }
    }

    ::glTranslatef((GLfloat)0, (GLfloat)0, (GLfloat)-z_shift);
}
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD

bool GLGizmoSlaSupports::is_mesh_update_necessary() const
{
#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    return (m_state == On) && (m_model_object != nullptr) && (m_model_object != m_old_model_object) && !m_model_object->instances.empty();
#else
    return m_state == On && m_model_object && !m_model_object->instances.empty() && !m_instance_matrix.isApprox(m_source_data.matrix);
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD

    //if (m_state != On || !m_model_object || m_model_object->instances.empty() || ! m_instance_matrix.isApprox(m_source_data.matrix))
    //    return false;

    // following should detect direct mesh changes (can be removed after the mesh is made completely immutable):
    /*const float* first_vertex = m_model_object->volumes.front()->get_convex_hull().first_vertex();
    Vec3d first_point((double)first_vertex[0], (double)first_vertex[1], (double)first_vertex[2]);
    if (first_point != m_source_data.mesh_first_point)
        return true;*/
}

void GLGizmoSlaSupports::update_mesh()
{
    Eigen::MatrixXf& V = m_V;
    Eigen::MatrixXi& F = m_F;
    // Composite mesh of all instances in the world coordinate system.
    // This mesh does not account for the possible Z up SLA offset.
    TriangleMesh mesh = m_model_object->raw_mesh();
    const stl_file& stl = mesh.stl;
    V.resize(3 * stl.stats.number_of_facets, 3);
    F.resize(stl.stats.number_of_facets, 3);
    for (unsigned int i=0; i<stl.stats.number_of_facets; ++i) {
        const stl_facet* facet = stl.facet_start+i;
        V(3*i+0, 0) = facet->vertex[0](0); V(3*i+0, 1) = facet->vertex[0](1); V(3*i+0, 2) = facet->vertex[0](2);
        V(3*i+1, 0) = facet->vertex[1](0); V(3*i+1, 1) = facet->vertex[1](1); V(3*i+1, 2) = facet->vertex[1](2);
        V(3*i+2, 0) = facet->vertex[2](0); V(3*i+2, 1) = facet->vertex[2](1); V(3*i+2, 2) = facet->vertex[2](2);
        F(i, 0) = 3*i+0;
        F(i, 1) = 3*i+1;
        F(i, 2) = 3*i+2;
    }

    m_AABB = igl::AABB<Eigen::MatrixXf,3>();
    m_AABB.init(m_V, m_F);

#if !ENABLE_SLA_SUPPORT_GIZMO_MOD
    m_source_data.matrix = m_instance_matrix;
#endif // !ENABLE_SLA_SUPPORT_GIZMO_MOD

    // we'll now reload Grabbers (selection might have changed):
    m_grabbers.clear();

    for (const Vec3f& point : m_model_object->sla_support_points) {
        m_grabbers.push_back(Grabber());
        m_grabbers.back().center = point.cast<double>();
    }
}

Vec3f GLGizmoSlaSupports::unproject_on_mesh(const Vec2d& mouse_pos)
{
    // if the gizmo doesn't have the V, F structures for igl, calculate them first:
#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    if (m_V.size() == 0)
#else
    if (m_V.size() == 0 || is_mesh_update_necessary())
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD
        update_mesh();

    Eigen::Matrix<GLint, 4, 1, Eigen::DontAlign> viewport;
    ::glGetIntegerv(GL_VIEWPORT, viewport.data());
    Eigen::Matrix<GLdouble, 4, 4, Eigen::DontAlign> modelview_matrix;
    ::glGetDoublev(GL_MODELVIEW_MATRIX, modelview_matrix.data());
    Eigen::Matrix<GLdouble, 4, 4, Eigen::DontAlign> projection_matrix;
    ::glGetDoublev(GL_PROJECTION_MATRIX, projection_matrix.data());

    Vec3d point1;
    Vec3d point2;
    ::gluUnProject(mouse_pos(0), viewport(3)-mouse_pos(1), 0.f, modelview_matrix.data(), projection_matrix.data(), viewport.data(), &point1(0), &point1(1), &point1(2));
    ::gluUnProject(mouse_pos(0), viewport(3)-mouse_pos(1), 1.f, modelview_matrix.data(), projection_matrix.data(), viewport.data(), &point2(0), &point2(1), &point2(2));

    igl::Hit hit;

#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    const GLCanvas3D::Selection& selection = m_parent.get_selection();
    const GLVolume* volume = selection.get_volume(*selection.get_volume_idxs().begin());
    double z_offset = volume->get_sla_shift_z();
#else
    double z_offset = m_parent.get_selection().get_volume(0)->get_sla_shift_z();
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD
    point1(2) -= z_offset;
	point2(2) -= z_offset;
#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    Transform3d inv = volume->get_instance_transformation().get_matrix().inverse();
#else
    Transform3d inv = m_instance_matrix.inverse();
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD
    point1 = inv * point1;
    point2 = inv * point2;

    if (!m_AABB.intersect_ray(m_V, m_F, point1.cast<float>(), (point2-point1).cast<float>(), hit))
        throw std::invalid_argument("unproject_on_mesh(): No intersection found.");

    int fid = hit.id;
    Vec3f bc(1-hit.u-hit.v, hit.u, hit.v);
    return bc(0) * m_V.row(m_F(fid, 0)) + bc(1) * m_V.row(m_F(fid, 1)) + bc(2)*m_V.row(m_F(fid, 2));
}

void GLGizmoSlaSupports::clicked_on_object(const Vec2d& mouse_position)
{
#if ENABLE_SLA_SUPPORT_GIZMO_MOD
    int instance_id = m_parent.get_selection().get_instance_idx();
    if (m_old_instance_id != instance_id)
    {
        bool something_selected = (m_old_instance_id != -1);
        m_old_instance_id = instance_id;
        if (something_selected)
            return;
    }
    if (instance_id == -1)
        return;
#endif // ENABLE_SLA_SUPPORT_GIZMO_MOD

    Vec3f new_pos;
    try {
        new_pos = unproject_on_mesh(mouse_position); // this can throw - we don't want to create a new grabber in that case
    }
    catch (...) { return; }

    m_grabbers.push_back(Grabber());
    m_grabbers.back().center = new_pos.cast<double>();
    m_model_object->sla_support_points.push_back(new_pos);

    // This should trigger the support generation
    // wxGetApp().plater()->reslice();

    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoSlaSupports::delete_current_grabber(bool delete_all)
{
    if (delete_all) {
        m_grabbers.clear();
        m_model_object->sla_support_points.clear();

        // This should trigger the support generation
        // wxGetApp().plater()->reslice();
    }
    else
        if (m_hover_id != -1) {
            m_grabbers.erase(m_grabbers.begin() + m_hover_id);
            m_model_object->sla_support_points.erase(m_model_object->sla_support_points.begin() + m_hover_id);
            m_hover_id = -1;

            // This should trigger the support generation
            // wxGetApp().plater()->reslice();
        }
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoSlaSupports::on_update(const UpdateData& data)
{
    if (m_hover_id != -1 && data.mouse_pos) {
        Vec3f new_pos;
        try {
            new_pos = unproject_on_mesh(Vec2d((*data.mouse_pos)(0), (*data.mouse_pos)(1)));
        }
        catch (...) { return; }
        m_grabbers[m_hover_id].center = new_pos.cast<double>();
        m_model_object->sla_support_points[m_hover_id] = new_pos;
        // Do not update immediately, wait until the mouse is released.
        // m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

#if !ENABLE_IMGUI
void GLGizmoSlaSupports::render_tooltip_texture() const {
    if (m_tooltip_texture.get_id() == 0)
        if (!m_tooltip_texture.load_from_file(resources_dir() + "/icons/sla_support_points_tooltip.png", false))
            return;
    if (m_reset_texture.get_id() == 0)
        if (!m_reset_texture.load_from_file(resources_dir() + "/icons/sla_support_points_reset.png", false))
            return;

    float zoom = m_parent.get_camera_zoom();
    float inv_zoom = (zoom != 0.0f) ? 1.0f / zoom : 0.0f;
    float gap = 30.0f * inv_zoom;

    const Size& cnv_size = m_parent.get_canvas_size();
    float l = gap - cnv_size.get_width()/2.f * inv_zoom;
    float r = l + (float)m_tooltip_texture.get_width() * inv_zoom;
    float b = gap - cnv_size.get_height()/2.f * inv_zoom;
    float t = b + (float)m_tooltip_texture.get_height() * inv_zoom;

    Rect reset_rect = m_parent.get_gizmo_reset_rect(m_parent, true);

    ::glDisable(GL_DEPTH_TEST);
    ::glPushMatrix();
    ::glLoadIdentity();
    GLTexture::render_texture(m_tooltip_texture.get_id(), l, r, b, t);
    GLTexture::render_texture(m_reset_texture.get_id(), reset_rect.get_left(), reset_rect.get_right(), reset_rect.get_bottom(), reset_rect.get_top());
    ::glPopMatrix();
    ::glEnable(GL_DEPTH_TEST);
}
#endif // not ENABLE_IMGUI


#if ENABLE_IMGUI
void GLGizmoSlaSupports::on_render_input_window(float x, float y, const GLCanvas3D::Selection& selection)
{
    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    m_imgui->set_next_window_bg_alpha(0.5f);
    m_imgui->begin(on_get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::PushItemWidth(100.0f);
    m_imgui->text(_(L("Left mouse click - add point")));
    m_imgui->text(_(L("Right mouse click - remove point")));
    m_imgui->text(" ");

    bool generate = m_imgui->button(_(L("Generate points automatically")));
    bool remove_all_clicked = m_imgui->button(_(L("Remove all points")) + (m_model_object == nullptr ? "" : " (" + std::to_string(m_model_object->sla_support_points.size())+")"));

    m_imgui->end();

    if (remove_all_clicked)
        delete_current_grabber(true);

    if (generate) {
        const DynamicPrintConfig& cfg = *wxGetApp().get_tab(Preset::TYPE_SLA_PRINT)->get_config();
        SLAAutoSupports::Config config;
        config.density_at_horizontal = cfg.opt_int("support_density_at_horizontal") / 10000.f;
        config.density_at_45 = cfg.opt_int("support_density_at_45") / 10000.f;
        config.minimal_z = cfg.opt_float("support_minimal_z");

        SLAAutoSupports sas(*m_model_object, config);
        sas.generate();
        m_grabbers.clear();
        for (const Vec3f& point : m_model_object->sla_support_points) {
            m_grabbers.push_back(Grabber());
            m_grabbers.back().center = point.cast<double>();
        }
    }

    if (remove_all_clicked || generate) {
        m_parent.reload_scene(true);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}
#endif // ENABLE_IMGUI

bool GLGizmoSlaSupports::on_is_activable(const GLCanvas3D::Selection& selection) const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA)
        && selection.is_from_single_instance();
}

bool GLGizmoSlaSupports::on_is_selectable() const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA);
}

std::string GLGizmoSlaSupports::on_get_name() const

{
    return L("SLA Support Points");
}



// GLGizmoCut

class GLGizmoCutPanel : public wxPanel
{
public:
    GLGizmoCutPanel(wxWindow *parent);

    void display(bool display);
private:
    bool m_active;
    wxCheckBox *m_cb_rotate;
    wxButton *m_btn_cut;
    wxButton *m_btn_cancel;
};

GLGizmoCutPanel::GLGizmoCutPanel(wxWindow *parent)
    : wxPanel(parent)
    , m_active(false)
    , m_cb_rotate(new wxCheckBox(this, wxID_ANY, _(L("Rotate lower part upwards"))))
    , m_btn_cut(new wxButton(this, wxID_OK, _(L("Perform cut"))))
    , m_btn_cancel(new wxButton(this, wxID_CANCEL, _(L("Cancel"))))
{
    enum { MARGIN = 5 };

    auto *sizer = new wxBoxSizer(wxHORIZONTAL);    

    auto *label = new wxStaticText(this, wxID_ANY, _(L("Cut object:")));
    sizer->Add(label, 0, wxALL | wxALIGN_CENTER, MARGIN);
    sizer->Add(m_cb_rotate, 0, wxALL | wxALIGN_CENTER, MARGIN);
    sizer->AddStretchSpacer();
    sizer->Add(m_btn_cut, 0, wxALL | wxALIGN_CENTER, MARGIN);
    sizer->Add(m_btn_cancel, 0, wxALL | wxALIGN_CENTER, MARGIN);

    SetSizer(sizer);
}

void GLGizmoCutPanel::display(bool display)
{
    Show(display);
    GetParent()->Layout();
}


const double GLGizmoCut::Offset = 10.0;
const double GLGizmoCut::Margin = 20.0;
const std::array<float, 3> GLGizmoCut::GrabberColor = { 1.0, 0.5, 0.0 };

GLGizmoCut::GLGizmoCut(GLCanvas3D& parent)
    : GLGizmoBase(parent)
    , m_cut_z(0.0)
    , m_max_z(0.0)
#if !ENABLE_IMGUI
    , m_panel(nullptr)
#endif // not ENABLE_IMGUI
    , m_keep_upper(true)
    , m_keep_lower(true)
    , m_rotate_lower(false)
{}

#if !ENABLE_IMGUI
void GLGizmoCut::create_external_gizmo_widgets(wxWindow *parent)
{
    wxASSERT(m_panel == nullptr);

    m_panel = new GLGizmoCutPanel(parent);
    parent->GetSizer()->Add(m_panel, 0, wxEXPAND);

    parent->Layout();
    parent->Fit();
    auto prev_heigh = parent->GetMinSize().GetHeight();
    parent->SetMinSize(wxSize(-1, std::max(prev_heigh, m_panel->GetSize().GetHeight())));

    m_panel->Hide();
    m_panel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        perform_cut(m_parent.get_selection());
    }, wxID_OK);
}
#endif // not ENABLE_IMGUI

bool GLGizmoCut::on_init()
{
    // TODO: icon

    std::string path = resources_dir() + "/icons/overlay/";

    if (!m_textures[Off].load_from_file(path + "cut_off.png", false)) {
        return false;
    }

    if (!m_textures[Hover].load_from_file(path + "cut_hover.png", false)) {
        return false;
    }

    if (!m_textures[On].load_from_file(path + "cut_on.png", false)) {
        return false;
    }

    m_grabbers.emplace_back();

    m_shortcut_key = WXK_CONTROL_C;

    return true;
}

std::string GLGizmoCut::on_get_name() const
{
    return L("Cut");
}

void GLGizmoCut::on_set_state()
{
    // Reset m_cut_z on gizmo activation
    if (get_state() == On) {
        m_cut_z = m_parent.get_selection().get_bounding_box().size()(2) / 2.0;
    }

#if !ENABLE_IMGUI
    // Display or hide the extra panel
    if (m_panel != nullptr) {
        m_panel->display(get_state() == On);
    }
#endif // not ENABLE_IMGUI
}

bool GLGizmoCut::on_is_activable(const GLCanvas3D::Selection& selection) const
{
    return selection.is_single_full_instance() && !selection.is_wipe_tower();
}

void GLGizmoCut::on_start_dragging(const GLCanvas3D::Selection& selection)
{
    if (m_hover_id == -1) { return; }

    const BoundingBoxf3& box = selection.get_bounding_box();
    m_start_z = m_cut_z;
    update_max_z(selection);
    m_drag_pos = m_grabbers[m_hover_id].center;
    m_drag_center = box.center();
    m_drag_center(2) = m_cut_z;
}

void GLGizmoCut::on_update(const UpdateData& data)
{
    if (m_hover_id != -1) {
        set_cut_z(m_start_z + calc_projection(data.mouse_ray));
    }
}

void GLGizmoCut::on_render(const GLCanvas3D::Selection& selection) const
{
    if (m_grabbers[0].dragging) {
        set_tooltip("Z: " + format(m_cut_z, 2));
    }

    update_max_z(selection);

    const BoundingBoxf3& box = selection.get_bounding_box();
    Vec3d plane_center = box.center();
    plane_center(2) = m_cut_z;

    const float min_x = box.min(0) - Margin;
    const float max_x = box.max(0) + Margin;
    const float min_y = box.min(1) - Margin;
    const float max_y = box.max(1) + Margin;
    ::glEnable(GL_DEPTH_TEST);
    ::glDisable(GL_CULL_FACE);
    ::glEnable(GL_BLEND);
    ::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw the cutting plane
    ::glBegin(GL_QUADS);
    ::glColor4f(0.8f, 0.8f, 0.8f, 0.5f);
    ::glVertex3f(min_x, min_y, plane_center(2));
    ::glVertex3f(max_x, min_y, plane_center(2));
    ::glVertex3f(max_x, max_y, plane_center(2));
    ::glVertex3f(min_x, max_y, plane_center(2));
    ::glEnd();

    ::glEnable(GL_CULL_FACE);
    ::glDisable(GL_BLEND);

    // TODO: draw cut part contour?

    // Draw the grabber and the connecting line
    m_grabbers[0].center = plane_center;
    m_grabbers[0].center(2) = plane_center(2) + Offset;

    ::glDisable(GL_DEPTH_TEST);
    ::glLineWidth(m_hover_id != -1 ? 2.0f : 1.5f);
    ::glColor3f(1.0, 1.0, 0.0);
    ::glBegin(GL_LINES);
    ::glVertex3dv(plane_center.data());
    ::glVertex3dv(m_grabbers[0].center.data());
    ::glEnd();

    std::copy(std::begin(GrabberColor), std::end(GrabberColor), m_grabbers[0].color);
    m_grabbers[0].render(m_hover_id == 0, box.max_size());
}

void GLGizmoCut::on_render_for_picking(const GLCanvas3D::Selection& selection) const
{
    ::glDisable(GL_DEPTH_TEST);

    render_grabbers_for_picking(selection.get_bounding_box());
}

#if ENABLE_IMGUI
void GLGizmoCut::on_render_input_window(float x, float y, const GLCanvas3D::Selection& selection)
{
    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    m_imgui->set_next_window_bg_alpha(0.5f);
    m_imgui->begin(_(L("Cut")), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::PushItemWidth(100.0f);
    bool _value_changed = ImGui::InputDouble("Z", &m_cut_z, 0.0f, 0.0f, "%.2f");

    m_imgui->checkbox(_(L("Keep upper part")), m_keep_upper);
    m_imgui->checkbox(_(L("Keep lower part")), m_keep_lower);
    m_imgui->checkbox(_(L("Rotate lower part upwards")), m_rotate_lower);

    const bool cut_clicked = m_imgui->button(_(L("Perform cut")));

    m_imgui->end();

    if (cut_clicked) {
        perform_cut(selection);
    }
}
#endif // ENABLE_IMGUI

void GLGizmoCut::update_max_z(const GLCanvas3D::Selection& selection) const
{
    m_max_z = selection.get_bounding_box().size()(2);
    set_cut_z(m_cut_z);
}

void GLGizmoCut::set_cut_z(double cut_z) const
{
    // Clamp the plane to the object's bounding box
    m_cut_z = std::max(0.0, std::min(m_max_z, cut_z));
}

void GLGizmoCut::perform_cut(const GLCanvas3D::Selection& selection)
{
    const auto instance_idx = selection.get_instance_idx();
    const auto object_idx = selection.get_object_idx();

    wxCHECK_RET(instance_idx >= 0 && object_idx >= 0, "GLGizmoCut: Invalid object selection");

    wxGetApp().plater()->cut(object_idx, instance_idx, m_cut_z, m_keep_upper, m_keep_lower, m_rotate_lower);
}

double GLGizmoCut::calc_projection(const Linef3& mouse_ray) const
{
    double projection = 0.0;

    const Vec3d starting_vec = m_drag_pos - m_drag_center;
    const double len_starting_vec = starting_vec.norm();
    if (len_starting_vec != 0.0)
    {
        Vec3d mouse_dir = mouse_ray.unit_vector();
        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
        // in our case plane normal and ray direction are the same (orthogonal view)
        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
        Vec3d inters = mouse_ray.a + (m_drag_pos - mouse_ray.a).dot(mouse_dir) / mouse_dir.squaredNorm() * mouse_dir;
        // vector from the starting position to the found intersection
        Vec3d inters_vec = inters - m_drag_pos;

        // finds projection of the vector along the staring direction
        projection = inters_vec.dot(starting_vec.normalized());
    }
    return projection;
}


} // namespace GUI
} // namespace Slic3r
