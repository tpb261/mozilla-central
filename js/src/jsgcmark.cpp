/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsgcmark.h"
#include "jsprf.h"
#include "jsscope.h"
#include "jsstr.h"

#include "jsobjinlines.h"
#include "jsscopeinlines.h"

#include "vm/String-inl.h"
#include "methodjit/MethodJIT.h"

/*
 * There are two mostly separate mark paths. The first is a fast path used
 * internally in the GC. The second is a slow path used for root marking and
 * for API consumers like the cycle collector or Class::trace implementations.
 *
 * The fast path uses explicit stacks. The basic marking process during a GC is
 * that all roots are pushed on to a mark stack, and then each item on the
 * stack is scanned (possibly pushing more stuff) until the stack is empty.
 *
 * PushMarkStack pushes a GC thing onto the mark stack. In some cases (shapes
 * or strings) it eagerly marks the object rather than pushing it. Popping and
 * scanning is done by the processMarkStackTop method. For efficiency reasons
 * like tail recursion elimination that method also implements the scanning of
 * objects. For other GC things it uses helper methods.
 *
 * Most of the marking code outside jsgcmark uses functions like MarkObject,
 * MarkString, etc. These functions check if an object is in the compartment
 * currently being GCed. If it is, they call PushMarkStack. Roots are pushed
 * this way as well as pointers traversed inside trace hooks (for things like
 * IteratorClass). It it always valid to call a MarkX function instead of
 * PushMarkStack, although it may be slower.
 *
 * The MarkX functions also handle non-GC object traversal. In this case, they
 * call a callback for each object visited. This is a recursive process; the
 * mark stacks are not involved. These callbacks may ask for the outgoing
 * pointers to be visited. Eventually, this leads to the MarkChildren functions
 * being called. These functions duplicate much of the functionality of
 * scanning functions, but they don't push onto an explicit stack.
 */

namespace js {
namespace gc {

static inline void
PushMarkStack(GCMarker *gcmarker, JSXML *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, JSObject *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, JSFunction *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, JSScript *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, const Shape *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, JSString *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, types::TypeObject *thing);

/*** Object Marking ***/

template<typename T>
static inline void
CheckMarkedThing(JSTracer *trc, T *thing)
{
    JS_ASSERT(trc);
    JS_ASSERT(thing);
    JS_ASSERT(trc->debugPrinter || trc->debugPrintArg);
    JS_ASSERT_IF(trc->runtime->gcCurrentCompartment, IS_GC_MARKING_TRACER(trc));

    JS_ASSERT(thing->isAligned());

    JS_ASSERT(thing->compartment());
    JS_ASSERT(thing->compartment()->rt == trc->runtime);
}

template<typename T>
void
MarkInternal(JSTracer *trc, T *thing)
{
    CheckMarkedThing(trc, thing);

    JSRuntime *rt = trc->runtime;

    JS_ASSERT_IF(rt->gcCheckCompartment,
                 thing->compartment() == rt->gcCheckCompartment ||
                 thing->compartment() == rt->atomsCompartment);

    /*
     * Don't mark things outside a compartment if we are in a per-compartment
     * GC.
     */
    if (!rt->gcCurrentCompartment || thing->compartment() == rt->gcCurrentCompartment) {
        if (IS_GC_MARKING_TRACER(trc)) {
            PushMarkStack(static_cast<GCMarker *>(trc), thing);
        } else {
            void *tmp = (void *)thing;
            trc->callback(trc, &tmp, GetGCThingTraceKind(thing));
            JS_ASSERT(tmp == thing);
        }
    }

#ifdef DEBUG
    trc->debugPrinter = NULL;
    trc->debugPrintArg = NULL;
#endif
}

template <typename T>
static void
MarkUnbarriered(JSTracer *trc, T *thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thing);
}

template <typename T>
static void
Mark(JSTracer *trc, const HeapPtr<T> &thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thing.get());
}

template <typename T>
static void
MarkRoot(JSTracer *trc, T *thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thing);
}

template <typename T>
static void
MarkRange(JSTracer *trc, size_t len, HeapPtr<T> *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        if (T *obj = vec[i]) {
            JS_SET_TRACING_INDEX(trc, name, i);
            MarkInternal(trc, obj);
        }
    }
}

template <typename T>
static void
MarkRootRange(JSTracer *trc, size_t len, T **vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkInternal(trc, vec[i]);
    }
}

#define DeclMarkerImpl(base, type)                                                                \
void                                                                                              \
Mark##base(JSTracer *trc, const HeapPtr<type> &thing, const char *name)                           \
{                                                                                                 \
    Mark<type>(trc, thing, name);                                                                 \
}                                                                                                 \
                                                                                                  \
void                                                                                              \
Mark##base##Root(JSTracer *trc, type *thing, const char *name)                                    \
{                                                                                                 \
    MarkRoot<type>(trc, thing, name);                                                             \
}                                                                                                 \
                                                                                                  \
void                                                                                              \
Mark##base##Unbarriered(JSTracer *trc, type *thing, const char *name)                             \
{                                                                                                 \
    MarkUnbarriered<type>(trc, thing, name);                                                      \
}                                                                                                 \
                                                                                                  \
void Mark##base##Range(JSTracer *trc, size_t len, HeapPtr<type> *vec, const char *name)           \
{                                                                                                 \
    MarkRange<type>(trc, len, vec, name);                                                         \
}                                                                                                 \
                                                                                                  \
void Mark##base##RootRange(JSTracer *trc, size_t len, type **vec, const char *name)               \
{                                                                                                 \
    MarkRootRange<type>(trc, len, vec, name);                                                     \
}                                                                                                 \

DeclMarkerImpl(BaseShape, BaseShape)
DeclMarkerImpl(Object, ArgumentsObject)
DeclMarkerImpl(Object, GlobalObject)
DeclMarkerImpl(Object, JSObject)
DeclMarkerImpl(Object, JSFunction)
DeclMarkerImpl(Script, JSScript)
DeclMarkerImpl(Shape, Shape)
DeclMarkerImpl(String, JSAtom)
DeclMarkerImpl(String, JSString)
DeclMarkerImpl(String, JSFlatString)
DeclMarkerImpl(String, JSLinearString)
DeclMarkerImpl(TypeObject, types::TypeObject)
#if JS_HAS_XML_SUPPORT
DeclMarkerImpl(XML, JSXML)
#endif

/*** Externally Typed Marking ***/

void
MarkKind(JSTracer *trc, void *thing, JSGCTraceKind kind)
{
    JS_ASSERT(thing);
    JS_ASSERT(kind == GetGCThingTraceKind(thing));
    switch (kind) {
      case JSTRACE_OBJECT:
        MarkInternal(trc, reinterpret_cast<JSObject *>(thing));
        break;
      case JSTRACE_STRING:
        MarkInternal(trc, reinterpret_cast<JSString *>(thing));
        break;
      case JSTRACE_SCRIPT:
        MarkInternal(trc, static_cast<JSScript *>(thing));
        break;
      case JSTRACE_SHAPE:
        MarkInternal(trc, reinterpret_cast<Shape *>(thing));
        break;
      case JSTRACE_BASE_SHAPE:
        MarkInternal(trc, reinterpret_cast<BaseShape *>(thing));
        break;
      case JSTRACE_TYPE_OBJECT:
        MarkInternal(trc, reinterpret_cast<types::TypeObject *>(thing));
        break;
#if JS_HAS_XML_SUPPORT
      case JSTRACE_XML:
        MarkInternal(trc, static_cast<JSXML *>(thing));
        break;
#endif
    }
}

void
MarkGCThingRoot(JSTracer *trc, void *thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    if (!thing)
        return;
    MarkKind(trc, thing, GetGCThingTraceKind(thing));
}

/*** ID Marking ***/

static inline void
MarkIdInternal(JSTracer *trc, const jsid &id)
{
    if (JSID_IS_STRING(id))
        MarkInternal(trc, JSID_TO_STRING(id));
    else if (JS_UNLIKELY(JSID_IS_OBJECT(id)))
        MarkInternal(trc, JSID_TO_OBJECT(id));
}

void
MarkId(JSTracer *trc, const HeapId &id, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkIdInternal(trc, id);
}

void
MarkIdRoot(JSTracer *trc, const jsid &id, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkIdInternal(trc, id);
}

void
MarkIdRange(JSTracer *trc, size_t len, HeapId *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkIdInternal(trc, vec[i]);
    }
}

void
MarkIdRootRange(JSTracer *trc, size_t len, jsid *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkIdInternal(trc, vec[i]);
    }
}

/*** Value Marking ***/

static inline void
MarkValueInternal(JSTracer *trc, Value *v)
{
    if (v->isMarkable()) {
        JS_ASSERT(v->toGCThing());
        return MarkKind(trc, v->toGCThing(), v->gcKind());
    }
}

void
MarkValue(JSTracer *trc, HeapValue *v, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkValueInternal(trc, v->unsafeGet());
}

void
MarkValueRoot(JSTracer *trc, Value *v, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkValueInternal(trc, v);
}

void
MarkValueRange(JSTracer *trc, size_t len, HeapValue *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkValueInternal(trc, vec[i].unsafeGet());
    }
}

void
MarkValueRootRange(JSTracer *trc, size_t len, Value *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkValueInternal(trc, &vec[i]);
    }
}

/*** Special Marking ***/

/*
 * The unioned HeapPtr stored in script->globalObj needs special treatment to
 * typecheck correctly.
 */
static void
MarkObject(JSTracer *trc, const HeapPtr<GlobalObject, JSScript *> &thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thing.get());
}

void
MarkShape(JSTracer *trc, const HeapPtr<const Shape> &thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, const_cast<Shape *>(thing.get()));
}

void
MarkValueUnbarriered(JSTracer *trc, Value *v, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkValueInternal(trc, v);
}

void
MarkCrossCompartmentValue(JSTracer *trc, HeapValue *v, const char *name)
{
    if (v->isMarkable()) {
        Cell *cell = (Cell *)v->toGCThing();
        JSRuntime *rt = trc->runtime;
        if (rt->gcCurrentCompartment && cell->compartment() != rt->gcCurrentCompartment)
            return;

        MarkValue(trc, v, name);
    }
}

/*** Push Mark Stack ***/

#define JS_COMPARTMENT_ASSERT(rt, thing)                                 \
    JS_ASSERT_IF((rt)->gcCurrentCompartment,                             \
                 (thing)->compartment() == (rt)->gcCurrentCompartment);

#define JS_COMPARTMENT_ASSERT_STR(rt, thing)                             \
    JS_ASSERT_IF((rt)->gcCurrentCompartment,                             \
                 (thing)->compartment() == (rt)->gcCurrentCompartment || \
                 (thing)->compartment() == (rt)->atomsCompartment);

static void
PushMarkStack(GCMarker *gcmarker, JSXML *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushXML(thing);
}

static void
PushMarkStack(GCMarker *gcmarker, JSObject *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushObject(thing);
}

static void
PushMarkStack(GCMarker *gcmarker, JSFunction *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushObject(thing);
}

static void
PushMarkStack(GCMarker *gcmarker, types::TypeObject *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushType(thing);
}

static void
MarkChildren(JSTracer *trc, JSScript *script);

static void
PushMarkStack(GCMarker *gcmarker, JSScript *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    /*
     * We mark scripts directly rather than pushing on the stack as they can
     * refer to other scripts only indirectly (like via nested functions) and
     * we cannot get to deep recursion.
     */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        MarkChildren(gcmarker, thing);
}

static void
ScanShape(GCMarker *gcmarker, const Shape *shape);

static void
PushMarkStack(GCMarker *gcmarker, const Shape *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    /* We mark shapes directly rather than pushing on the stack. */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        ScanShape(gcmarker, thing);
}

static inline void
ScanBaseShape(GCMarker *gcmarker, BaseShape *base);

static void
PushMarkStack(GCMarker *gcmarker, BaseShape *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    /* We mark base shapes directly rather than pushing on the stack. */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        ScanBaseShape(gcmarker, thing);
}

static void
ScanShape(GCMarker *gcmarker, const Shape *shape)
{
  restart:
    PushMarkStack(gcmarker, shape->base());

    jsid id = shape->maybePropid();
    if (JSID_IS_STRING(id))
        PushMarkStack(gcmarker, JSID_TO_STRING(id));
    else if (JS_UNLIKELY(JSID_IS_OBJECT(id)))
        PushMarkStack(gcmarker, JSID_TO_OBJECT(id));

    shape = shape->previous();
    if (shape && shape->markIfUnmarked(gcmarker->getMarkColor()))
        goto restart;
}

static inline void
ScanBaseShape(GCMarker *gcmarker, BaseShape *base)
{
    base->assertConsistency();

    if (base->hasGetterObject())
        PushMarkStack(gcmarker, base->getterObject());

    if (base->hasSetterObject())
        PushMarkStack(gcmarker, base->setterObject());

    if (JSObject *parent = base->getObjectParent())
        PushMarkStack(gcmarker, parent);

    /*
     * All children of the owned base shape are consistent with its
     * unowned one, thus we do not need to trace through children of the
     * unowned base shape.
     */
    if (base->isOwned()) {
        UnownedBaseShape *unowned = base->baseUnowned();
        JS_ASSERT(base->compartment() == unowned->compartment());
        unowned->markIfUnmarked(gcmarker->getMarkColor());
    }
}

static inline void
ScanLinearString(GCMarker *gcmarker, JSLinearString *str)
{
    JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, str);
    JS_ASSERT(str->isMarked());

    /*
     * Add extra asserts to confirm the static type to detect incorrect string
     * mutations.
     */
    JS_ASSERT(str->JSString::isLinear());
    while (str->isDependent()) {
        str = str->asDependent().base();
        JS_ASSERT(str->JSString::isLinear());
        JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, str);
        if (!str->markIfUnmarked())
            break;
    }
}

/*
 * The function tries to scan the whole rope tree using the marking stack as
 * temporary storage. If that becomes full, the unscanned ropes are added to
 * the delayed marking list. When the function returns, the marking stack is
 * at the same depth as it was on entry. This way we avoid using tags when
 * pushing ropes to the stack as ropes never leaks to other users of the
 * stack. This also assumes that a rope can only point to other ropes or
 * linear strings, it cannot refer to GC things of other types.
 */
static void
ScanRope(GCMarker *gcmarker, JSRope *rope)
{
    uintptr_t *savedTos = gcmarker->stack.tos;
    for (;;) {
        JS_ASSERT(GetGCThingTraceKind(rope) == JSTRACE_STRING);
        JS_ASSERT(rope->JSString::isRope());
        JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, rope);
        JS_ASSERT(rope->isMarked());
        JSRope *next = NULL;

        JSString *right = rope->rightChild();
        if (right->markIfUnmarked()) {
            if (right->isLinear())
                ScanLinearString(gcmarker, &right->asLinear());
            else
                next = &right->asRope();
        }

        JSString *left = rope->leftChild();
        if (left->markIfUnmarked()) {
            if (left->isLinear()) {
                ScanLinearString(gcmarker, &left->asLinear());
            } else {
                /*
                 * When both children are ropes, set aside the right one to
                 * scan it later.
                 */
                if (next && !gcmarker->stack.push(reinterpret_cast<uintptr_t>(next)))
                    gcmarker->delayMarkingChildren(next);
                next = &left->asRope();
            }
        }
        if (next) {
            rope = next;
        } else if (savedTos != gcmarker->stack.tos) {
            JS_ASSERT(savedTos < gcmarker->stack.tos);
            rope = reinterpret_cast<JSRope *>(gcmarker->stack.pop());
        } else {
            break;
        }
    }
    JS_ASSERT(savedTos == gcmarker->stack.tos);
 }

static inline void
ScanString(GCMarker *gcmarker, JSString *str)
{
    if (str->isLinear())
        ScanLinearString(gcmarker, &str->asLinear());
    else
        ScanRope(gcmarker, &str->asRope());
}

static inline void
PushMarkStack(GCMarker *gcmarker, JSString *str)
{
    JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, str);

    /*
     * As string can only refer to other strings we fully scan its GC graph
     * using the explicit stack when navigating the rope tree to avoid
     * dealing with strings on the stack in drainMarkStack.
     */
    if (str->markIfUnmarked())
        ScanString(gcmarker, str);
}

static inline void
PushValueArray(GCMarker *gcmarker, JSObject* obj, HeapValue *start, HeapValue *end)
{
    JS_ASSERT(start <= end);
    uintptr_t tagged = reinterpret_cast<uintptr_t>(obj) | GCMarker::ValueArrayTag;
    uintptr_t startAddr = reinterpret_cast<uintptr_t>(start);
    uintptr_t endAddr = reinterpret_cast<uintptr_t>(end);

    /* Push in the reverse order so obj will be on top. */
    if (!gcmarker->stack.push(endAddr, startAddr, tagged)) {
        /*
         * If we cannot push the array, we trigger delay marking for the whole
         * object.
         */
        gcmarker->delayMarkingChildren(obj);
    }
}

void
MarkChildren(JSTracer *trc, JSObject *obj)
{
    MarkTypeObject(trc, obj->typeFromGC(), "type");

    Shape *shape = obj->lastProperty();
    MarkShapeUnbarriered(trc, shape, "shape");

    Class *clasp = shape->getObjectClass();
    if (clasp->trace)
        clasp->trace(trc, obj);

    if (shape->isNative()) {
        uint32_t nslots = obj->slotSpan();
        for (uint32_t i = 0; i < nslots; i++) {
            JS_SET_TRACING_DETAILS(trc, js_PrintObjectSlotName, obj, i);
            MarkValueInternal(trc, obj->nativeGetSlotRef(i).unsafeGet());
        }
    }
}

static void
MarkChildren(JSTracer *trc, JSString *str)
{
    /*
     * We use custom barriers in JSString, so it's safe to use unbarriered
     * marking here.
     */
    if (str->isDependent()) {
        MarkStringUnbarriered(trc, str->asDependent().base(), "base");
    } else if (str->isRope()) {
        JSRope &rope = str->asRope();
        MarkStringUnbarriered(trc, rope.leftChild(), "left child");
        MarkStringUnbarriered(trc, rope.rightChild(), "right child");
    }
}

static void
MarkChildren(JSTracer *trc, JSScript *script)
{
    CheckScript(script, NULL);

    JS_ASSERT_IF(trc->runtime->gcCheckCompartment,
                 script->compartment() == trc->runtime->gcCheckCompartment);

    for (uint32_t i = 0; i < script->natoms; ++i) {
        if (JSAtom *p = script->atoms[i])
            MarkStringUnbarriered(trc, p, "atom");
    }

    if (JSScript::isValidOffset(script->objectsOffset)) {
        JSObjectArray *objarray = script->objects();
        MarkObjectRange(trc, objarray->length, objarray->vector, "objects");
    }

    if (JSScript::isValidOffset(script->regexpsOffset)) {
        JSObjectArray *objarray = script->regexps();
        MarkObjectRange(trc, objarray->length, objarray->vector, "objects");
    }

    if (JSScript::isValidOffset(script->constOffset)) {
        JSConstArray *constarray = script->consts();
        MarkValueRange(trc, constarray->length, constarray->vector, "consts");
    }

    if (script->function())
        MarkObjectUnbarriered(trc, script->function(), "function");

    if (!script->isCachedEval && script->globalObject)
        MarkObject(trc, script->globalObject, "object");

    if (IS_GC_MARKING_TRACER(trc) && script->filename)
        js_MarkScriptFilename(script->filename);

    script->bindings.trace(trc);

    if (script->types)
        script->types->trace(trc);

    if (script->hasAnyBreakpointsOrStepMode())
        script->markTrapClosures(trc);
}

static void
MarkChildren(JSTracer *trc, const Shape *shape)
{
    MarkBaseShapeUnbarriered(trc, shape->base(), "base");
    MarkId(trc, shape->maybePropid(), "propid");
    if (shape->previous())
        MarkShape(trc, shape->previous(), "parent");
}

static inline void
MarkBaseShapeGetterSetter(JSTracer *trc, BaseShape *base)
{
    if (base->hasGetterObject())
        MarkObjectUnbarriered(trc, base->getterObject(), "getter");
    if (base->hasSetterObject())
        MarkObjectUnbarriered(trc, base->setterObject(), "setter");
}

static void
MarkChildren(JSTracer *trc, BaseShape *base)
{
    MarkBaseShapeGetterSetter(trc, base);
    if (base->isOwned())
        MarkBaseShapeUnbarriered(trc, base->baseUnowned(), "base");

    if (JSObject *parent = base->getObjectParent())
        MarkObjectUnbarriered(trc, parent, "parent");
}

/*
 * This function is used by the cycle collector to trace through the
 * children of a BaseShape (and its baseUnowned(), if any). The cycle
 * collector does not directly care about BaseShapes, so only the
 * getter, setter, and parent are marked. Furthermore, the parent is
 * marked only if it isn't the same as prevParent, which will be
 * updated to the current shape's parent.
 */
inline void
MarkCycleCollectorChildren(JSTracer *trc, BaseShape *base, JSObject **prevParent)
{
    JS_ASSERT(base);

    /*
     * The cycle collector does not need to trace unowned base shapes,
     * as they have the same getter, setter and parent as the original
     * base shape.
     */
    base->assertConsistency();

    MarkBaseShapeGetterSetter(trc, base);

    JSObject *parent = base->getObjectParent();
    if (parent && parent != *prevParent) {
        MarkObjectUnbarriered(trc, parent, "parent");
        *prevParent = parent;
    }
}

/*
 * This function is used by the cycle collector to trace through a
 * shape. The cycle collector does not care about shapes or base
 * shapes, so those are not marked. Instead, any shapes or base shapes
 * that are encountered have their children marked. Stack space is
 * bounded. If two shapes in a row have the same parent pointer, the
 * parent pointer will only be marked once.
 */
void
MarkCycleCollectorChildren(JSTracer *trc, const Shape *shape)
{
    JSObject *prevParent = NULL;
    do {
        MarkCycleCollectorChildren(trc, shape->base(), &prevParent);
        MarkId(trc, shape->maybePropid(), "propid");
        shape = shape->previous();
    } while (shape);
}

static void
ScanTypeObject(GCMarker *gcmarker, types::TypeObject *type)
{
    if (!type->singleton) {
        unsigned count = type->getPropertyCount();
        for (unsigned i = 0; i < count; i++) {
            types::Property *prop = type->getProperty(i);
            if (prop && JSID_IS_STRING(prop->id))
                PushMarkStack(gcmarker, JSID_TO_STRING(prop->id));
        }
    }

    if (type->proto)
        PushMarkStack(gcmarker, type->proto);

    if (type->newScript) {
        PushMarkStack(gcmarker, type->newScript->fun);
        PushMarkStack(gcmarker, type->newScript->shape);
    }

    if (type->interpretedFunction)
        PushMarkStack(gcmarker, type->interpretedFunction);

    if (type->singleton && !type->lazy())
        PushMarkStack(gcmarker, type->singleton);

    if (type->interpretedFunction)
        PushMarkStack(gcmarker, type->interpretedFunction);
}

static void
MarkChildren(JSTracer *trc, types::TypeObject *type)
{
    if (!type->singleton) {
        unsigned count = type->getPropertyCount();
        for (unsigned i = 0; i < count; i++) {
            types::Property *prop = type->getProperty(i);
            if (prop)
                MarkId(trc, prop->id, "type_prop");
        }
    }

    if (type->proto)
        MarkObject(trc, type->proto, "type_proto");

    if (type->singleton && !type->lazy())
        MarkObject(trc, type->singleton, "type_singleton");

    if (type->newScript) {
        MarkObject(trc, type->newScript->fun, "type_new_function");
        MarkShape(trc, type->newScript->shape, "type_new_shape");
    }

    if (type->interpretedFunction)
        MarkObject(trc, type->interpretedFunction, "type_function");
}

#ifdef JS_HAS_XML_SUPPORT
static void
MarkChildren(JSTracer *trc, JSXML *xml)
{
    js_TraceXML(trc, xml);
}
#endif

} /* namespace gc */

using namespace js::gc;

inline void
GCMarker::processMarkStackTop()
{
    /*
     * The function uses explicit goto and implements the scanning of the
     * object directly. It allows to eliminate the tail recursion and
     * significantly improve the marking performance, see bug 641025.
     */
    HeapValue *vp, *end;
    JSObject *obj;

    uintptr_t addr = stack.pop();
    uintptr_t tag = addr & StackTagMask;
    addr &= ~StackTagMask;

    if (tag == ValueArrayTag) {
        JS_STATIC_ASSERT(ValueArrayTag == 0);
        JS_ASSERT(!(addr & Cell::CellMask));
        obj = reinterpret_cast<JSObject *>(addr);
        uintptr_t addr2 = stack.pop();
        uintptr_t addr3 = stack.pop();
        JS_ASSERT(addr2 <= addr3);
        JS_ASSERT((addr3 - addr2) % sizeof(Value) == 0);
        vp = reinterpret_cast<HeapValue *>(addr2);
        end = reinterpret_cast<HeapValue *>(addr3);
        goto scan_value_array;
    }

    if (tag == ObjectTag) {
        obj = reinterpret_cast<JSObject *>(addr);
        goto scan_obj;
    }

    if (tag == TypeTag) {
        ScanTypeObject(this, reinterpret_cast<types::TypeObject *>(addr));
    } else {
        JS_ASSERT(tag == XmlTag);
        MarkChildren(this, reinterpret_cast<JSXML *>(addr));
    }
    return;

  scan_value_array:
    JS_ASSERT(vp <= end);
    while (vp != end) {
        const Value &v = *vp++;
        if (v.isString()) {
            JSString *str = v.toString();
            if (str->markIfUnmarked())
                ScanString(this, str);
        } else if (v.isObject()) {
            JSObject *obj2 = &v.toObject();
            if (obj2->markIfUnmarked(getMarkColor())) {
                PushValueArray(this, obj, vp, end);
                obj = obj2;
                goto scan_obj;
            }
        }
    }
    return;

  scan_obj:
    {
        types::TypeObject *type = obj->typeFromGC();
        PushMarkStack(this, type);

        Shape *shape = obj->lastProperty();
        PushMarkStack(this, shape);

        /* Call the trace hook if necessary. */
        Class *clasp = shape->getObjectClass();
        if (clasp->trace) {
            if (clasp == &ArrayClass) {
                JS_ASSERT(!shape->isNative());
                vp = obj->getDenseArrayElements();
                end = vp + obj->getDenseArrayInitializedLength();
                goto scan_value_array;
            }
            clasp->trace(this, obj);
        }

        if (!shape->isNative())
            return;

        unsigned nslots = obj->slotSpan();
        vp = obj->fixedSlots();
        if (obj->slots) {
            unsigned nfixed = obj->numFixedSlots();
            if (nslots > nfixed) {
                PushValueArray(this, obj, vp, vp + nfixed);
                vp = obj->slots;
                end = vp + (nslots - nfixed);
                goto scan_value_array;
            }
        }
        JS_ASSERT(nslots <= obj->numFixedSlots());
        end = vp + nslots;
        goto scan_value_array;
    }
}

void
GCMarker::drainMarkStack()
{
    JSRuntime *rt = runtime;
    rt->gcCheckCompartment = rt->gcCurrentCompartment;

    for (;;) {
        while (!stack.isEmpty())
            processMarkStackTop();
        if (!hasDelayedChildren())
            break;

        /*
         * Mark children of things that caused too deep recursion during the
         * above tracing. Don't do this until we're done with everything
         * else.
         */
        markDelayedChildren();
    }

    rt->gcCheckCompartment = NULL;
}

void
TraceChildren(JSTracer *trc, void *thing, JSGCTraceKind kind)
{
    switch (kind) {
      case JSTRACE_OBJECT:
        MarkChildren(trc, static_cast<JSObject *>(thing));
        break;

      case JSTRACE_STRING:
        MarkChildren(trc, static_cast<JSString *>(thing));
        break;

      case JSTRACE_SCRIPT:
        MarkChildren(trc, static_cast<JSScript *>(thing));
        break;

      case JSTRACE_SHAPE:
        MarkChildren(trc, static_cast<Shape *>(thing));
        break;

      case JSTRACE_BASE_SHAPE:
        MarkChildren(trc, static_cast<BaseShape *>(thing));
        break;

      case JSTRACE_TYPE_OBJECT:
        MarkChildren(trc, (types::TypeObject *)thing);
        break;

#if JS_HAS_XML_SUPPORT
      case JSTRACE_XML:
        MarkChildren(trc, static_cast<JSXML *>(thing));
        break;
#endif
    }
}

void
CallTracer(JSTracer *trc, void *thing, JSGCTraceKind kind)
{
    JS_ASSERT(thing);
    MarkKind(trc, thing, kind);
}

} /* namespace js */
