/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "modules/skottie/src/SkottieAdapter.h"

#include "include/core/SkFont.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkMatrix44.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/private/SkTo.h"
#include "include/utils/Sk3D.h"
#include "modules/skottie/src/SkottieValue.h"
#include "modules/sksg/include/SkSGDraw.h"
#include "modules/sksg/include/SkSGGradient.h"
#include "modules/sksg/include/SkSGGroup.h"
#include "modules/sksg/include/SkSGPaint.h"
#include "modules/sksg/include/SkSGPath.h"
#include "modules/sksg/include/SkSGRect.h"
#include "modules/sksg/include/SkSGTransform.h"
#include "modules/sksg/include/SkSGTrimEffect.h"

#include <cmath>
#include <utility>

namespace skottie {

namespace internal {

DiscardableAdaptorBase::DiscardableAdaptorBase() = default;

void DiscardableAdaptorBase::setAnimators(sksg::AnimatorList&& animators) {
    fAnimators = std::move(animators);
}

void DiscardableAdaptorBase::onTick(float t) {
    for (auto& animator : fAnimators) {
        animator->tick(t);
    }

    this->onSync();
}

} // namespace internal

RRectAdapter::RRectAdapter(sk_sp<sksg::RRect> wrapped_node)
    : fRRectNode(std::move(wrapped_node)) {}

RRectAdapter::~RRectAdapter() = default;

void RRectAdapter::apply() {
    // BM "position" == "center position"
    auto rr = SkRRect::MakeRectXY(SkRect::MakeXYWH(fPosition.x() - fSize.width() / 2,
                                                   fPosition.y() - fSize.height() / 2,
                                                   fSize.width(), fSize.height()),
                                  fRadius.width(),
                                  fRadius.height());
   fRRectNode->setRRect(rr);
}

TransformAdapter2D::TransformAdapter2D(sk_sp<sksg::Matrix<SkMatrix>> matrix)
    : fMatrixNode(std::move(matrix)) {}

TransformAdapter2D::~TransformAdapter2D() = default;

SkMatrix TransformAdapter2D::totalMatrix() const {
    SkMatrix t = SkMatrix::MakeTrans(-fAnchorPoint.x(), -fAnchorPoint.y());

    t.postScale(fScale.x() / 100, fScale.y() / 100); // 100% based
    t.postRotate(fRotation);
    t.postTranslate(fPosition.x(), fPosition.y());
    // TODO: skew

    return t;
}

void TransformAdapter2D::apply() {
    fMatrixNode->setMatrix(this->totalMatrix());
}

TransformAdapter3D::Vec3::Vec3(const VectorValue& v) {
    fX = v.size() > 0 ? v[0] : 0;
    fY = v.size() > 1 ? v[1] : 0;
    fZ = v.size() > 2 ? v[2] : 0;
}

TransformAdapter3D::TransformAdapter3D()
    : fMatrixNode(sksg::Matrix<SkMatrix44>::Make(SkMatrix::I())) {}

TransformAdapter3D::~TransformAdapter3D() = default;

sk_sp<sksg::Transform> TransformAdapter3D::refTransform() const {
    return fMatrixNode;
}

SkMatrix44 TransformAdapter3D::totalMatrix() const {
    SkMatrix44 t;

    t.setTranslate(-fAnchorPoint.fX, -fAnchorPoint.fY, -fAnchorPoint.fZ);
    t.postScale(fScale.fX / 100, fScale.fY / 100, fScale.fZ / 100);

    SkMatrix44 r;
    r.setRotateDegreesAbout(0, 0, 1, fRotation.fZ);
    t.postConcat(r);
    r.setRotateDegreesAbout(0, 1, 0, fRotation.fY);
    t.postConcat(r);
    r.setRotateDegreesAbout(1, 0, 0, fRotation.fX);
    t.postConcat(r);

    t.postTranslate(fPosition.fX, fPosition.fY, fPosition.fZ);

    return t;
}

void TransformAdapter3D::apply() {
    fMatrixNode->setMatrix(this->totalMatrix());
}

CameraAdapter:: CameraAdapter(const SkSize& viewport_size)
    : fViewportSize(viewport_size) {}

CameraAdapter::~CameraAdapter() = default;

sk_sp<CameraAdapter> CameraAdapter::MakeDefault(const SkSize &viewport_size) {
    auto adapter = sk_make_sp<CameraAdapter>(viewport_size);

    static constexpr float kDefaultAEZoom = 879.13f;
    const auto center = SkVector::Make(viewport_size.width()  * 0.5f,
                                       viewport_size.height() * 0.5f);
    adapter->setZoom(kDefaultAEZoom);
    adapter->setAnchorPoint(TransformAdapter3D::Vec3({center.fX, center.fY, 0}));
    adapter->setPosition   (TransformAdapter3D::Vec3({center.fX, center.fY, -kDefaultAEZoom}));

    return adapter;
}

SkMatrix44 CameraAdapter::totalMatrix() const {
    // Camera parameters:
    //
    //   * location          -> position attribute
    //   * point of interest -> anchor point attribute
    //   * orientation       -> rotation attribute
    //
    SkPoint3 pos = { this->getPosition().fX,
                     this->getPosition().fY,
                    -this->getPosition().fZ },
             poi = { this->getAnchorPoint().fX,
                     this->getAnchorPoint().fY,
                    -this->getAnchorPoint().fZ },
              up = { 0, 1, 0 };

    // Initial camera vector.
    SkMatrix44 cam_t;
    Sk3LookAt(&cam_t, pos, poi, up);

    // Rotation origin is camera position.
    {
        SkMatrix44 rot;
        rot.setRotateDegreesAbout(1, 0, 0,  this->getRotation().fX);
        cam_t.postConcat(rot);
        rot.setRotateDegreesAbout(0, 1, 0,  this->getRotation().fY);
        cam_t.postConcat(rot);
        rot.setRotateDegreesAbout(0, 0, 1, -this->getRotation().fZ);
        cam_t.postConcat(rot);
    }

    // Flip world Z, as it is opposite of what Sk3D expects.
    cam_t.preScale(1, 1, -1);

    // View parameters:
    //
    //   * size     -> composition size (TODO: AE seems to base it on width only?)
    //   * distance -> "zoom" camera attribute
    //
    const auto view_size     = SkTMax(fViewportSize.width(), fViewportSize.height()),
               view_distance = this->getZoom(),
               view_angle    = std::atan(sk_ieee_float_divide(view_size * 0.5f, view_distance));

    SkMatrix44 persp_t;
    Sk3Perspective(&persp_t, 0, view_distance, 2 * view_angle);
    persp_t.postScale(view_size * 0.5f, view_size * 0.5f, 1);

    SkMatrix44 t;
    t.setTranslate(fViewportSize.width() * 0.5f, fViewportSize.height() * 0.5f, 0);
    t.preConcat(persp_t);
    t.preConcat(cam_t);

    return t;
}

RepeaterAdapter::RepeaterAdapter(sk_sp<sksg::RenderNode> repeater_node, Composite composite)
    : fRepeaterNode(repeater_node)
    , fComposite(composite)
    , fRoot(sksg::Group::Make()) {}

RepeaterAdapter::~RepeaterAdapter() = default;

void RepeaterAdapter::apply() {
    static constexpr SkScalar kMaxCount = 512;
    const auto count = static_cast<size_t>(SkTPin(fCount, 0.0f, kMaxCount) + 0.5f);

    const auto& compute_transform = [this] (size_t index) {
        const auto t = fOffset + index;

        // Position, scale & rotation are "scaled" by index/offset.
        SkMatrix m = SkMatrix::MakeTrans(-fAnchorPoint.x(),
                                         -fAnchorPoint.y());
        m.postScale(std::pow(fScale.x() * .01f, fOffset),
                    std::pow(fScale.y() * .01f, fOffset));
        m.postRotate(t * fRotation);
        m.postTranslate(t * fPosition.x() + fAnchorPoint.x(),
                        t * fPosition.y() + fAnchorPoint.y());

        return m;
    };

    // TODO: start/end opacity support.

    // TODO: we can avoid rebuilding all the fragments in most cases.
    fRoot->clear();
    for (size_t i = 0; i < count; ++i) {
        const auto insert_index = (fComposite == Composite::kAbove) ? i : count - i - 1;
        fRoot->addChild(sksg::TransformEffect::Make(fRepeaterNode,
                                                    compute_transform(insert_index)));
    }
}

PolyStarAdapter::PolyStarAdapter(sk_sp<sksg::Path> wrapped_node, Type t)
    : fPathNode(std::move(wrapped_node))
    , fType(t) {}

PolyStarAdapter::~PolyStarAdapter() = default;

void PolyStarAdapter::apply() {
    static constexpr int kMaxPointCount = 100000;
    const auto count = SkToUInt(SkTPin(SkScalarRoundToInt(fPointCount), 0, kMaxPointCount));
    const auto arc   = sk_ieee_float_divide(SK_ScalarPI * 2, count);

    const auto pt_on_circle = [](const SkPoint& c, SkScalar r, SkScalar a) {
        return SkPoint::Make(c.x() + r * std::cos(a),
                             c.y() + r * std::sin(a));
    };

    // TODO: inner/outer "roundness"?

    SkPath poly;

    auto angle = SkDegreesToRadians(fRotation - 90);
    poly.moveTo(pt_on_circle(fPosition, fOuterRadius, angle));
    poly.incReserve(fType == Type::kStar ? count * 2 : count);

    for (unsigned i = 0; i < count; ++i) {
        if (fType == Type::kStar) {
            poly.lineTo(pt_on_circle(fPosition, fInnerRadius, angle + arc * 0.5f));
        }
        angle += arc;
        poly.lineTo(pt_on_circle(fPosition, fOuterRadius, angle));
    }

    poly.close();
    fPathNode->setPath(poly);
}

GradientAdapter::GradientAdapter(sk_sp<sksg::Gradient> grad, size_t colorStopCount)
    : fGradient(std::move(grad))
    , fColorStopCount(colorStopCount) {}

void GradientAdapter::apply() {
    this->onApply();

    // Gradient color stops are specified as a consolidated float vector holding:
    //
    //   a) an (optional) array of color/RGB stop records (t, r, g, b)
    //
    // followed by
    //
    //   b) an (optional) array of opacity/alpha stop records (t, a)
    //
    struct   ColorRec { float t, r, g, b; };
    struct OpacityRec { float t, a;       };

    // The number of color records is explicit (fColorStopCount),
    // while the number of opacity stops is implicit (based on the size of fStops).
    //
    // |fStops| holds ColorRec x |fColorStopCount| + OpacityRec x N
    const auto c_count = fColorStopCount,
               c_size  = c_count * 4,
               o_count = (fStops.size() - c_size) / 2;
    if (fStops.size() < c_size || fStops.size() != (c_count * 4 + o_count * 2)) {
        // apply() may get called before the stops are set, so only log when we have some stops.
        if (!fStops.empty()) {
            SkDebugf("!! Invalid gradient stop array size: %zu\n", fStops.size());
        }
        return;
    }

    const auto* current_c = reinterpret_cast<const ColorRec*>(fStops.data());
    const auto*     end_c = current_c + c_count;
    const auto* current_o = reinterpret_cast<const OpacityRec*>(end_c);
    const auto*     end_o = current_o + o_count;

    // TODO: Gradient/ColorStop should use 4f.
    sksg::Gradient::ColorStop prev_stop = { 0.0f, SK_ColorBLACK };
    if (current_c < end_c) {
        prev_stop.fColor = SkColor4f{ current_c->r, current_c->g, current_c->b, 1.0 }.toSkColor();
    }
    if (current_o < end_o) {
        prev_stop.fColor = SkColorSetA(prev_stop.fColor, SkScalarRoundToInt(current_o->a * 255));
    }

    auto lerp = [](float a, float b, float t) { return a + t * (b - a); };

    auto next_stop = [&]() -> sksg::Gradient::ColorStop {
        const auto prev_c = SkColor4f::FromColor(prev_stop.fColor);

        const uint8_t has_color_stop   = SkToU8(current_c < end_c),
                      has_opacity_stop = SkToU8(current_o < end_o);

        switch (has_color_stop | (has_opacity_stop << 1)) {
            case 0x01: {
                // Color-only stop.
                sksg::Gradient::ColorStop cs{
                    current_c->t,
                    SkColor4f{ current_c->r, current_c->g, current_c->b, prev_c.fA }.toSkColor()
                };

                current_c++;
                return cs;
            }
            case 0x02: {
                // Opacity-only stop.
                sksg::Gradient::ColorStop cs{
                    current_o->t,
                    SkColor4f{ prev_c.fR, prev_c.fG, prev_c.fB, current_o->a }.toSkColor()
                };

               current_o++;
               return cs;
            }
            case 0x03: {
                // Separate color and opacity stops.
                // Merge-sort the two arrays, LERP-ing intermediate channel values as needed.

                auto c     = SkColor4f{ current_c->r, current_c->g, current_c->b, current_o->a };
                auto t_rgb = current_c->t,
                     t_a   = current_o->t;

                if (SkScalarNearlyEqual(t_rgb, t_a)) {
                    // Coincident color and opacity stops: no LERP needed, consume both.
                    current_c++;
                    current_o++;

                    return { t_rgb, c.toSkColor() };
                }

                if (t_rgb < t_a) {
                    // Color stop followed by opacity stop: LERP alpha, consume the color stop.
                    const auto rel_t = SkTPin(sk_ieee_float_divide(t_rgb - prev_stop.fPosition,
                                                                   t_a - prev_stop.fPosition),
                                              0.0f, 1.0f);
                    c.fA = lerp(prev_c.fA, c.fA, rel_t);

                    current_c++;

                    return { t_rgb, c.toSkColor() };
                } else {
                    // Opacity stop followed by color stop: LERP r/g/b, consume the opacity stop.
                    const auto rel_t = SkTPin(sk_ieee_float_divide(t_a - prev_stop.fPosition,
                                                                   t_rgb - prev_stop.fPosition),
                                              0.0f, 1.0f);
                    c.fR = lerp(prev_c.fR, c.fR, rel_t);
                    c.fG = lerp(prev_c.fG, c.fG, rel_t);
                    c.fB = lerp(prev_c.fB, c.fB, rel_t);

                    current_o++;

                    return { t_a, c.toSkColor() };
                }
            }

            default: SkUNREACHABLE;
        }
    };

    std::vector<sksg::Gradient::ColorStop> stops;
    stops.reserve(c_count);

    while (current_c < end_c || current_o < end_o) {
        prev_stop = next_stop();
        stops.push_back(prev_stop);
    }

    stops.shrink_to_fit();
    fGradient->setColorStops(std::move(stops));
}

LinearGradientAdapter::LinearGradientAdapter(sk_sp<sksg::LinearGradient> grad, size_t stopCount)
    : INHERITED(std::move(grad), stopCount) {}

void LinearGradientAdapter::onApply() {
    auto* grad = static_cast<sksg::LinearGradient*>(fGradient.get());
    grad->setStartPoint(this->startPoint());
    grad->setEndPoint(this->endPoint());
}

RadialGradientAdapter::RadialGradientAdapter(sk_sp<sksg::RadialGradient> grad, size_t stopCount)
    : INHERITED(std::move(grad), stopCount) {}

void RadialGradientAdapter::onApply() {
    auto* grad = static_cast<sksg::RadialGradient*>(fGradient.get());
    grad->setStartCenter(this->startPoint());
    grad->setEndCenter(this->startPoint());
    grad->setStartRadius(0);
    grad->setEndRadius(SkPoint::Distance(this->startPoint(), this->endPoint()));
}

TrimEffectAdapter::TrimEffectAdapter(sk_sp<sksg::TrimEffect> trimEffect)
    : fTrimEffect(std::move(trimEffect)) {
    SkASSERT(fTrimEffect);
}

TrimEffectAdapter::~TrimEffectAdapter() = default;

void TrimEffectAdapter::apply() {
    // BM semantics: start/end are percentages, offset is "degrees" (?!).
    const auto  start = fStart  / 100,
                  end = fEnd    / 100,
               offset = fOffset / 360;

    auto startT = SkTMin(start, end) + offset,
          stopT = SkTMax(start, end) + offset;
    auto   mode = SkTrimPathEffect::Mode::kNormal;

    if (stopT - startT < 1) {
        startT -= SkScalarFloorToScalar(startT);
        stopT  -= SkScalarFloorToScalar(stopT);

        if (startT > stopT) {
            using std::swap;
            swap(startT, stopT);
            mode = SkTrimPathEffect::Mode::kInverted;
        }
    } else {
        startT = 0;
        stopT  = 1;
    }

    fTrimEffect->setStart(startT);
    fTrimEffect->setStop(stopT);
    fTrimEffect->setMode(mode);
}

} // namespace skottie
