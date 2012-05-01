/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jsnum_h___
#define jsnum_h___

#include "mozilla/FloatingPoint.h"

#include <math.h>

#include "jsobj.h"

#include "vm/NumericConversions.h"

extern double js_NaN;
extern double js_PositiveInfinity;
extern double js_NegativeInfinity;

namespace js {

extern bool
InitRuntimeNumberState(JSRuntime *rt);

extern void
FinishRuntimeNumberState(JSRuntime *rt);

} /* namespace js */

/* Initialize the Number class, returning its prototype object. */
extern JSObject *
js_InitNumberClass(JSContext *cx, JSObject *obj);

/*
 * String constants for global function names, used in jsapi.c and jsnum.c.
 */
extern const char js_isNaN_str[];
extern const char js_isFinite_str[];
extern const char js_parseFloat_str[];
extern const char js_parseInt_str[];

class JSString;
class JSFixedString;

extern JSString *
js_IntToString(JSContext *cx, int i);

/*
 * When base == 10, this function implements ToString() as specified by
 * ECMA-262-5 section 9.8.1; but note that it handles integers specially for
 * performance.  See also js::NumberToCString().
 */
extern JSString *
js_NumberToString(JSContext *cx, double d);

namespace js {

/*
 * Convert an integer or double (contained in the given value) to a string and
 * append to the given buffer.
 */
extern bool JS_FASTCALL
NumberValueToStringBuffer(JSContext *cx, const Value &v, StringBuffer &sb);

/* Same as js_NumberToString, different signature. */
extern JSFixedString *
NumberToString(JSContext *cx, double d);

extern JSFixedString *
IndexToString(JSContext *cx, uint32_t index);

/*
 * Usually a small amount of static storage is enough, but sometimes we need
 * to dynamically allocate much more.  This struct encapsulates that.
 * Dynamically allocated memory will be freed when the object is destroyed.
 */
struct ToCStringBuf
{
    /*
     * The longest possible result that would need to fit in sbuf is
     * (-0x80000000).toString(2), which has length 33.  Longer cases are
     * possible, but they'll go in dbuf.
     */
    static const size_t sbufSize = 34;
    char sbuf[sbufSize];
    char *dbuf;

    ToCStringBuf();
    ~ToCStringBuf();
};

/*
 * Convert a number to a C string.  When base==10, this function implements
 * ToString() as specified by ECMA-262-5 section 9.8.1.  It handles integral
 * values cheaply.  Return NULL if we ran out of memory.  See also
 * js_NumberToCString().
 */
extern char *
NumberToCString(JSContext *cx, ToCStringBuf *cbuf, double d, int base = 10);

/*
 * The largest positive integer such that all positive integers less than it
 * may be precisely represented using the IEEE-754 double-precision format.
 */
const double DOUBLE_INTEGRAL_PRECISION_LIMIT = uint64_t(1) << 53;

/*
 * Compute the positive integer of the given base described immediately at the
 * start of the range [start, end) -- no whitespace-skipping, no magical
 * leading-"0" octal or leading-"0x" hex behavior, no "+"/"-" parsing, just
 * reading the digits of the integer.  Return the index one past the end of the
 * digits of the integer in *endp, and return the integer itself in *dp.  If
 * base is 10 or a power of two the returned integer is the closest possible
 * double; otherwise extremely large integers may be slightly inaccurate.
 *
 * If [start, end) does not begin with a number with the specified base,
 * *dp == 0 and *endp == start upon return.
 */
extern bool
GetPrefixInteger(JSContext *cx, const jschar *start, const jschar *end, int base,
                 const jschar **endp, double *dp);

/* ES5 9.3 ToNumber, overwriting *vp with the appropriate number value. */
JS_ALWAYS_INLINE bool
ToNumber(JSContext *cx, Value *vp)
{
    if (vp->isNumber())
        return true;
    double d;
    extern bool ToNumberSlow(JSContext *cx, js::Value v, double *dp);
    if (!ToNumberSlow(cx, *vp, &d))
        return false;
    vp->setNumber(d);
    return true;
}

/*
 * Convert a value to a uint32_t, according to the ECMA rules for
 * ToUint32. Return converted value in *out on success, !ok on
 * failure.
 */

JS_ALWAYS_INLINE bool
ToUint32(JSContext *cx, const js::Value &v, uint32_t *out)
{
    if (v.isInt32()) {
        *out = (uint32_t)v.toInt32();
        return true;
    }
    extern bool ToUint32Slow(JSContext *cx, const js::Value &v, uint32_t *ip);
    return ToUint32Slow(cx, v, out);
}

/*
 * Convert a value to a number, then to an int32_t if it fits by rounding to
 * nearest. Return converted value in *out on success, !ok on failure. As a
 * side effect, *vp will be mutated to match *out.
 */
JS_ALWAYS_INLINE bool
NonstandardToInt32(JSContext *cx, const js::Value &v, int32_t *out)
{
    if (v.isInt32()) {
        *out = v.toInt32();
        return true;
    }
    extern bool NonstandardToInt32Slow(JSContext *cx, const js::Value &v, int32_t *ip);
    return NonstandardToInt32Slow(cx, v, out);
}

/*
 * Convert a value to a number, then to a uint16_t according to the ECMA rules
 * for ToUint16. Return converted value on success, !ok on failure. v must be a
 * copy of a rooted value.
 */
JS_ALWAYS_INLINE bool
ValueToUint16(JSContext *cx, const js::Value &v, uint16_t *out)
{
    if (v.isInt32()) {
        *out = uint16_t(v.toInt32());
        return true;
    }
    extern bool ValueToUint16Slow(JSContext *cx, const js::Value &v, uint16_t *out);
    return ValueToUint16Slow(cx, v, out);
}

JSBool
num_parseInt(JSContext *cx, unsigned argc, Value *vp);

}  /* namespace js */

/*
 * Similar to strtod except that it replaces overflows with infinities of the
 * correct sign, and underflows with zeros of the correct sign.  Guaranteed to
 * return the closest double number to the given input in dp.
 *
 * Also allows inputs of the form [+|-]Infinity, which produce an infinity of
 * the appropriate sign.  The case of the "Infinity" string must match exactly.
 * If the string does not contain a number, set *ep to s and return 0.0 in dp.
 * Return false if out of memory.
 */
extern JSBool
js_strtod(JSContext *cx, const jschar *s, const jschar *send,
          const jschar **ep, double *dp);

extern JSBool
js_num_valueOf(JSContext *cx, unsigned argc, js::Value *vp);

namespace js {

static JS_ALWAYS_INLINE bool
ValueFitsInInt32(const Value &v, int32_t *pi)
{
    if (v.isInt32()) {
        *pi = v.toInt32();
        return true;
    }
    return v.isDouble() && MOZ_DOUBLE_IS_INT32(v.toDouble(), pi);
}

/*
 * Returns true if the given value is definitely an index: that is, the value
 * is a number that's an unsigned 32-bit integer.
 *
 * This method prioritizes common-case speed over accuracy in every case.  It
 * can produce false negatives (but not false positives): some values which are
 * indexes will be reported not to be indexes by this method.  Users must
 * consider this possibility when using this method.
 */
static JS_ALWAYS_INLINE bool
IsDefinitelyIndex(const Value &v, uint32_t *indexp)
{
    if (v.isInt32() && v.toInt32() >= 0) {
        *indexp = v.toInt32();
        return true;
    }

    int32_t i;
    if (v.isDouble() && MOZ_DOUBLE_IS_INT32(v.toDouble(), &i) && i >= 0) {
        *indexp = uint32_t(i);
        return true;
    }

    return false;
}

/* ES5 9.4 ToInteger. */
static inline bool
ToInteger(JSContext *cx, const js::Value &v, double *dp)
{
    if (v.isInt32()) {
        *dp = v.toInt32();
        return true;
    }
    if (v.isDouble()) {
        *dp = v.toDouble();
    } else {
        extern bool ToNumberSlow(JSContext *cx, Value v, double *dp);
        if (!ToNumberSlow(cx, v, dp))
            return false;
    }
    *dp = ToInteger(*dp);
    return true;
}

inline bool
SafeAdd(int32_t one, int32_t two, int32_t *res)
{
    *res = one + two;
    int64_t ores = (int64_t)one + (int64_t)two;
    return ores == (int64_t)*res;
}

inline bool
SafeSub(int32_t one, int32_t two, int32_t *res)
{
    *res = one - two;
    int64_t ores = (int64_t)one - (int64_t)two;
    return ores == (int64_t)*res;
}

inline bool
SafeMul(int32_t one, int32_t two, int32_t *res)
{
    *res = one * two;
    int64_t ores = (int64_t)one * (int64_t)two;
    return ores == (int64_t)*res;
}

} /* namespace js */

#endif /* jsnum_h___ */
