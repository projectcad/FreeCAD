/***************************************************************************
 *   Copyright (c) Jürgen Riegel          (juergen.riegel@web.de) 2002     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include "PreCompiled.h"

#ifndef _PreComp_
# include <gp_Trsf.hxx>
# include <gp_Ax1.hxx>
# include <BRepBuilderAPI_MakeShape.hxx>
# include <BRepAlgoAPI_Fuse.hxx>
# include <BRepAlgoAPI_Common.hxx>
# include <TopTools_ListIteratorOfListOfShape.hxx>
# include <TopExp.hxx>
# include <TopExp_Explorer.hxx>
# include <TopTools_IndexedMapOfShape.hxx>
# include <Standard_Failure.hxx>
# include <TopoDS_Face.hxx>
# include <gp_Dir.hxx>
# include <gp_Pln.hxx> // for Precision::Confusion()
# include <Bnd_Box.hxx>
# include <BRepBndLib.hxx>
# include <BRepExtrema_DistShapeShape.hxx>
# include <BRepAdaptor_Curve.hxx>
# include <TopoDS.hxx>
#endif


#include <boost/algorithm/string/predicate.hpp>
#include <sstream>
#include <Base/Console.h>
#include <Base/Writer.h>
#include <Base/Reader.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>
#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <App/Application.h>
#include <App/FeaturePythonPyImp.h>
#include <App/Document.h>

#include "PartPyCXX.h"
#include "PartFeature.h"
#include "PartFeaturePy.h"
#include "TopoShapePy.h"

using namespace Part;

FC_LOG_LEVEL_INIT("Part",true,true);

PROPERTY_SOURCE(Part::Feature, App::GeoFeature)


Feature::Feature(void) 
{
    ADD_PROPERTY(Shape, (TopoDS_Shape()));
    ADD_PROPERTY_TYPE(ColoredElements, (0), "",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_ReadOnly|App::Prop_Output),"");
}

Feature::~Feature()
{
}

short Feature::mustExecute(void) const
{
    return GeoFeature::mustExecute();
}

App::DocumentObjectExecReturn *Feature::recompute(void)
{
    try {
        return App::GeoFeature::recompute();
    }
    catch (Standard_Failure& e) {

        App::DocumentObjectExecReturn* ret = new App::DocumentObjectExecReturn(e.GetMessageString());
        if (ret->Why.empty()) ret->Why = "Unknown OCC exception";
        return ret;
    }
}

App::DocumentObjectExecReturn *Feature::execute(void)
{
    this->Shape.touch();
    return GeoFeature::execute();
}

PyObject *Feature::getPyObject(void)
{
    if (PythonObject.is(Py::_None())){
        // ref counter is set to 1
        PythonObject = Py::Object(new PartFeaturePy(this),true);
    }
    return Py::new_reference_to(PythonObject); 
}

App::DocumentObject *Feature::getSubObject(const char *subname, 
        PyObject **pyObj, Base::Matrix4D *pmat, bool transform, int depth) const
{
    // having '.' inside subname means it is referencing some children object,
    // instead of any sub-element from ourself
    if(subname && !Data::ComplexGeoData::isMappedElement(subname) && strchr(subname,'.')) 
        return App::DocumentObject::getSubObject(subname,pyObj,pmat,transform,depth);

    if(pmat && transform)
        *pmat *= Placement.getValue().toMatrix();

    if(!pyObj) {
#if 0
        if(subname==0 || *subname==0 || Shape.getShape().hasSubShape(subname))
            return const_cast<Feature*>(this);
        return nullptr;
#else
        // TopoShape::hasSubShape is kind of slow, let's cut outself some slack here.
        return const_cast<Feature*>(this);
#endif
    }

    try {
        TopoShape ts(Shape.getShape());
        bool doTransform = pmat && *pmat!=ts.getTransform();
        if(doTransform)
            ts.setTransform(Base::Matrix4D());
        if(subname && *subname)
            ts = ts.getSubTopoShape(subname);
        if(doTransform && !ts.isNull()) {
            static int sCopy = -1; 
            if(sCopy<0) {
                ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
                        "User parameter:BaseApp/Preferences/Mod/Part/General");
                sCopy = hGrp->GetBool("CopySubShape",false)?1:0;
            }
            bool copy = sCopy?true:false;
            if(!copy) {
                // Work around OCC bug on transforming circular edge with an
                // offseted surface. The bug probably affect other shape type,
                // too.
                TopExp_Explorer exp(ts.getShape(),TopAbs_EDGE);
                if(exp.More()) {
                    auto edge = TopoDS::Edge(exp.Current());
                    exp.Next();
                    if(!exp.More()) {
                        BRepAdaptor_Curve curve(edge);
                        copy = curve.GetType() == GeomAbs_Circle;
                    }
                }
            }
            ts.transformShape(*pmat,copy,true);
        }
        *pyObj =  Py::new_reference_to(shape2pyshape(ts));
        return const_cast<Feature*>(this);
    }catch(Standard_Failure &e) {
        std::ostringstream str;
        Standard_CString msg = e.GetMessageString();
        str << typeid(e).name() << " ";
        if (msg) {str << msg;}
        else     {str << "No OCCT Exception Message";}
        str << ": " << getNameInDocument();
        if (subname) 
            str << '.' << subname;
        FC_ERR(str.str());
        return 0;
    }
}

static std::pair<long,std::string> getElementSource(App::DocumentObject *owner, 
        TopoShape shape, std::string name, char type)
{
    long tag = 0;
    while(1) {
        std::string original;
        std::vector<std::string> history;
        long t = shape.getElementHistory(name.c_str(),&original,&history);
        if(!t)
            break;
        auto obj = owner->getDocument()->getObjectByID(t);
        if(!obj)
            break;
        if(type) {
            for(auto &hist : history) {
                if(shape.elementType(hist.c_str())!=type)
                    return std::make_pair(tag,name);
            }
        }
        owner = 0;
        shape = Part::Feature::getTopoShape(obj,0,false,0,&owner); 
        if(!owner || shape.isNull())
            break;
        if(type && shape.elementType(original.c_str())!=type)
            break;
        name = original;
        tag = t;
    }
    return std::make_pair(tag,name);
}

std::list<Part::Feature::HistoryItem> 
Feature::getElementHistory(App::DocumentObject *feature, 
        const char *name, bool recursive, bool sameType)
{
    std::list<HistoryItem> ret;
    TopoShape shape = getTopoShape(feature);
    std::string mapped = shape.getElementName(name,true);
    char element_type=0;
    if(sameType)
        element_type = shape.elementType(name);
    do {
        std::string original;
        ret.emplace_back(feature,mapped.c_str());
        long tag = shape.getElementHistory(mapped.c_str(),&original,&ret.back().intermediates);
        App::DocumentObject *obj;
        obj = tag?feature->getLinkedObject(true)->getDocument()->getObjectByID(tag):0;
        if(!recursive) {
            ret.emplace_back(obj,original.c_str());
            ret.back().tag = tag;
            return ret;
        }
        if(!obj)
            break;
        if(element_type) {
            for(auto &hist : ret.back().intermediates) {
                if(shape.elementType(hist.c_str())!=element_type)
                    return ret;
            }
        }
        feature = obj;
        shape = Feature::getTopoShape(feature);
        mapped = original;
        if(element_type && shape.elementType(original.c_str())!=element_type)
            break;
    }while(feature);
    return ret;
}

std::vector<std::pair<std::string,std::string> > 
Feature::getRelatedElements(App::DocumentObject *obj, const char *name, bool sameType)
{
    auto owner = obj;
    auto shape = getTopoShape(obj,0,false,0,&owner); 
    auto ret = shape.getRelatedElements(name,sameType); 
    if(ret.size()) {
        FC_LOG("topo shape returns " << ret.size() << " related elements");
        return ret;
    }

    char element_type = shape.elementType(name);
    TopAbs_ShapeEnum type = TopoShape::shapeType(element_type,true);
    if(type == TopAbs_SHAPE)
        return ret;
    auto source = getElementSource(owner,shape,name,element_type);
    if(!source.first ||
       shape.getRelatedElementsCached(source.second.c_str(),source.first,sameType,ret))
        return ret;

    auto shapetype = TopoShape::shapeName(type);
    std::ostringstream ss;
    for(size_t i=1;i<=shape.countSubShapes(type);++i) {
        ss.str("");
        ss << shapetype << i;
        std::string element(ss.str());
        auto mapped = shape.getElementName(element.c_str(),true);
        if(mapped == element.c_str())
            continue;
        if(getElementSource(owner,shape,mapped,sameType?element_type:0) == source)
            ret.emplace_back(mapped,element);
    }
    shape.cacheRelatedElements(source.second.c_str(),source.first,sameType,ret);
    FC_LOG("topo shape history returns " << ret.size() << " related elements");
    return ret;
}

TopoDS_Shape Feature::getShape(const App::DocumentObject *obj, const char *subname, 
        bool needSubElement, Base::Matrix4D *pmat, App::DocumentObject **powner, 
        bool resolveLink, bool transform) 
{
    return getTopoShape(obj,subname,needSubElement,pmat,powner,resolveLink,transform,true).getShape();
}

TopoShape Feature::getTopoShape(const App::DocumentObject *obj, const char *subname, 
        bool needSubElement, Base::Matrix4D *pmat, App::DocumentObject **powner, 
        bool resolveLink, bool transform, bool noElementMap)
{
    if(!obj) return TopoShape();

    PyObject *pyobj = 0;
    Base::Matrix4D mat;
    if(pmat) mat = *pmat;
    if(powner) *powner = 0;

    std::string _subname;
    auto subelement = Data::ComplexGeoData::findElementName(subname);
    if(!needSubElement && subname) {
        // strip out element name if not needed
        if(subelement && *subelement) {
            _subname = std::string(subname,subelement);
            subname = _subname.c_str();
        }
    }
    auto owner = obj->getSubObject(subname,&pyobj,&mat,transform);
    auto linked = owner;
    long tag = 0;
    App::StringHasherRef hasher;
    if(owner) {
        tag = owner->getID();
        hasher = owner->getDocument()->getStringHasher();
        linked = owner->getLinkedObject(true,(pmat&&resolveLink)?&mat:0,false);
        if(!linked)
            linked = owner;
        if(powner) 
            *powner = resolveLink?linked:owner;
    }
    if(pmat)
        *pmat = mat;

    if(pyobj && PyObject_TypeCheck(pyobj,&TopoShapePy::Type)) {
        auto shape = *static_cast<TopoShapePy*>(pyobj)->getTopoShapePtr();
        if(!noElementMap && tag && owner!=linked)
            shape.reTagElementMap(tag,hasher);
        Py_DECREF(pyobj);
        return shape;
    }

    Py_XDECREF(pyobj);

    if(!owner)
        return TopoShape();

    // nothing can be done if there is sub-element references
    if(needSubElement && subelement && *subelement)
        return TopoShape();

    // If no subelement reference, then try to create compound of sub objects
    std::vector<TopoShape> shapes;
    for(auto name : owner->getSubObjects()) {
        if(name.empty()) continue;
        int visible;
        if(name[name.size()-1] == '.')
            visible = owner->isElementVisible(name.substr(0,name.size()-1).c_str());
        else
            visible = owner->isElementVisible(name.c_str());
        if(visible==0)
            continue;
        if(name[name.size()-1]!='.')
            name += '.';
        DocumentObject *subObj = 0;
        auto shape = getTopoShape(owner,name.c_str(),false,0,&subObj,false,false,noElementMap);
        if(visible<0 && subObj && !subObj->Visibility.getValue())
            continue;
        if(!shape.isNull()) {
            if(noElementMap) 
                shapes.push_back(TopoShape(shape.getShape()));
            else
                shapes.push_back(shape);
        }
    }
    if(shapes.empty()) 
        return TopoShape();
    TopoShape ts;
    ts.makECompound(shapes);
    ts.transformShape(mat,false,true);
    if(!noElementMap && tag && owner!=linked) 
        ts.reTagElementMap(tag,hasher);
    return ts;
}

App::DocumentObject *Feature::getShapeOwner(const App::DocumentObject *obj, const char *subname)
{
    if(!obj) return 0;
    auto owner = obj->getSubObject(subname);
    if(owner) {
        auto linked = owner->getLinkedObject(true);
        if(linked)
            owner = linked;
    }
    return owner;
}

void Feature::onChanged(const App::Property* prop)
{
    // if the placement has changed apply the change to the point data as well
    if (prop == &this->Placement) {
        TopoShape& shape = const_cast<TopoShape&>(this->Shape.getShape());
        shape.setTransform(this->Placement.getValue().toMatrix());
    }
    // if the point data has changed check and adjust the transformation as well
    else if (prop == &this->Shape) {
        if (this->isRecomputing()) {
            TopoShape& shape = const_cast<TopoShape&>(this->Shape.getShape());
            shape.setTransform(this->Placement.getValue().toMatrix());
        }
        else {
            Base::Placement p;
            // shape must not be null to override the placement
            if (!this->Shape.getValue().IsNull()) {
                p.fromMatrix(this->Shape.getShape().getTransform());
                if (p != this->Placement.getValue())
                    this->Placement.setValue(p);
            }
        }
    }
    
    GeoFeature::onChanged(prop);
}

TopLoc_Location Feature::getLocation() const
{
    Base::Placement pl = this->Placement.getValue();
    Base::Rotation rot(pl.getRotation());
    Base::Vector3d axis;
    double angle;
    rot.getValue(axis, angle);
    gp_Trsf trf;
    trf.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(axis.x, axis.y, axis.z)), angle);
    trf.SetTranslationPart(gp_Vec(pl.getPosition().x,pl.getPosition().y,pl.getPosition().z));
    return TopLoc_Location(trf);
}

    /// returns the type name of the ViewProvider
const char* Feature::getViewProviderName(void) const {
    return "PartGui::ViewProviderPart";
}

const App::PropertyComplexGeoData* Feature::getPropertyOfGeometry() const
{
    return &Shape;
}

// ---------------------------------------------------------

PROPERTY_SOURCE(Part::FilletBase, Part::Feature)

FilletBase::FilletBase()
{
    ADD_PROPERTY(Base,(0));
    ADD_PROPERTY(Edges,(0,0,0));
    ADD_PROPERTY_TYPE(EdgeLinks,(0), 0, 
            (App::PropertyType)(App::Prop_ReadOnly|App::Prop_Hidden),0);
    Edges.setSize(0);
}

short FilletBase::mustExecute() const
{
    if (Base.isTouched() || Edges.isTouched() || EdgeLinks.isTouched())
        return 1;
    return 0;
}

void FilletBase::onChanged(const App::Property *prop) {
    if(getDocument() && !getDocument()->testStatus(App::Document::Restoring)) {
        if(prop == &Edges || prop == &Base) {
            if(!prop->testStatus(App::Property::User3))
                syncEdgeLink();
        }
    }
    Feature::onChanged(prop);
}

void FilletBase::onDocumentRestored() {
    if(EdgeLinks.getSubValues().empty())
        syncEdgeLink();
    Feature::onDocumentRestored();
}

void FilletBase::syncEdgeLink() {
    if(!Base.getValue() || !Edges.getSize()) {
        EdgeLinks.setValue(0);
        return;
    }
    std::vector<std::string> subs;
    std::string sub("Edge");
    for(auto &info : Edges.getValues()) 
        subs.emplace_back(sub+std::to_string(info.edgeid));
    EdgeLinks.setValue(Base.getValue(),subs);
}

void FilletBase::onUpdateElementReference(const App::Property *prop) {
    if(prop!=&EdgeLinks || !getNameInDocument())
        return;
    auto values = Edges.getValues();
    const auto &subs = EdgeLinks.getSubValues();
    for(size_t i=0;i<values.size();++i) {
        if(i>=subs.size()) {
            FC_WARN("fillet edge count mismatch in object " << getNameInDocument());
            break;
        }
        int idx = 0;
        sscanf(subs[i].c_str(),"Edge%d",&idx);
        if(idx) 
            values[i].edgeid = idx;
        else
            FC_WARN("invalid fillet edge link '" << subs[i] << "' in object " 
                    << getNameInDocument());
    }
    Edges.setStatus(App::Property::User3,true);
    Edges.setValues(values);
    Edges.setStatus(App::Property::User3,false);
}

// ---------------------------------------------------------

PROPERTY_SOURCE(Part::FeatureExt, Part::Feature)



namespace App {
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(Part::FeaturePython, Part::Feature)
template<> const char* Part::FeaturePython::getViewProviderName(void) const {
    return "PartGui::ViewProviderPython";
}
template<> PyObject* Part::FeaturePython::getPyObject(void) {
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new FeaturePythonPyT<Part::PartFeaturePy>(this),true);
    }
    return Py::new_reference_to(PythonObject);
}
/// @endcond

// explicit template instantiation
template class PartExport FeaturePythonT<Part::Feature>;
}

// ----------------------------------------------------------------

#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <gce_MakeLin.hxx>
#include <BRepIntCurveSurface_Inter.hxx>
#include <IntCurveSurface_IntersectionPoint.hxx>
#include <gce_MakeDir.hxx>

std::vector<Part::cutFaces> Part::findAllFacesCutBy(
        const TopoDS_Shape& shape, const TopoDS_Shape& face, const gp_Dir& dir)
{
    // Find the centre of gravity of the face
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face,props);
    gp_Pnt cog = props.CentreOfMass();

    // create a line through the centre of gravity
    gp_Lin line = gce_MakeLin(cog, dir);

    // Find intersection of line with all faces of the shape
    std::vector<cutFaces> result;
    BRepIntCurveSurface_Inter mkSection;
    // TODO: Less precision than Confusion() should be OK?

    for (mkSection.Init(shape, line, Precision::Confusion()); mkSection.More(); mkSection.Next()) {
        gp_Pnt iPnt = mkSection.Pnt();
        double dsq = cog.SquareDistance(iPnt);

        if (dsq < Precision::Confusion())
            continue; // intersection with original face

        // Find out which side of the original face the intersection is on
        gce_MakeDir mkDir(cog, iPnt);
        if (!mkDir.IsDone())
            continue; // some error (appears highly unlikely to happen, though...)

        if (mkDir.Value().IsOpposite(dir, Precision::Confusion()))
            continue; // wrong side of face (opposite to extrusion direction)

        cutFaces newF;
        newF.face = mkSection.Face();
        newF.distsq = dsq;
        result.push_back(newF);
    }

    return result;
}

bool Part::checkIntersection(const TopoDS_Shape& first, const TopoDS_Shape& second,
                             const bool quick, const bool touch_is_intersection) {

    Bnd_Box first_bb, second_bb;
    BRepBndLib::Add(first, first_bb);
    first_bb.SetGap(0);
    BRepBndLib::Add(second, second_bb);
    second_bb.SetGap(0);

    // Note: This test fails if the objects are touching one another at zero distance
    
    // Improving reliability: If it fails sometimes when touching and touching is intersection, 
    // then please check further unless the user asked for a quick potentially unreliable result
    if (first_bb.IsOut(second_bb) && !touch_is_intersection)
        return false; // no intersection
    if (quick && !first_bb.IsOut(second_bb))
        return true; // assumed intersection

    // Try harder
    
    // This has been disabled because of:
    // https://www.freecadweb.org/tracker/view.php?id=3065
    
    //extrema method
    /*BRepExtrema_DistShapeShape extrema(first, second);
    if (!extrema.IsDone())
      return true;
    if (extrema.Value() > Precision::Confusion())
      return false;
    if (extrema.InnerSolution())
      return true;
    
    //here we should have touching shapes.
    if (touch_is_intersection)
    {

    //non manifold condition. 1 has to be a face
    for (int index = 1; index < extrema.NbSolution() + 1; ++index)
    {
        if (extrema.SupportTypeShape1(index) == BRepExtrema_IsInFace || extrema.SupportTypeShape2(index) == BRepExtrema_IsInFace)
            return true;
        }
      return false;
    }
    else
      return false;*/
    
    //boolean method.
    
    if (touch_is_intersection) {
        // If both shapes fuse to a single solid, then they intersect
        BRepAlgoAPI_Fuse mkFuse(first, second);
        if (!mkFuse.IsDone())
            return false;
        if (mkFuse.Shape().IsNull())
            return false;

        // Did we get one or two solids?
        TopExp_Explorer xp;
        xp.Init(mkFuse.Shape(),TopAbs_SOLID);
        if (xp.More()) {
            // At least one solid
            xp.Next();
            return (xp.More() == Standard_False);
        } else {
            return false;
        }
    } else {
        // If both shapes have common material, then they intersect
        BRepAlgoAPI_Common mkCommon(first, second);
        if (!mkCommon.IsDone())
            return false;
        if (mkCommon.Shape().IsNull())
            return false;

        // Did we get a solid?
        TopExp_Explorer xp;
        xp.Init(mkCommon.Shape(),TopAbs_SOLID);
        return (xp.More() == Standard_True);
    }
    
}
