# Copyright 2019 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import web_idl

from . import name_style
from .code_node import Likeliness
from .code_node import SymbolDefinitionNode
from .code_node import SymbolNode
from .code_node import TextNode
from .code_node_cxx import CxxIfElseNode
from .code_node_cxx import CxxLikelyIfNode
from .code_node_cxx import CxxUnlikelyIfNode
from .codegen_format import format_template as _format


def blink_class_name(idl_definition):
    """
    Returns the class name of Blink implementation.
    """
    class_name = idl_definition.code_generator_info.receiver_implemented_as
    if class_name:
        return class_name

    if isinstance(idl_definition,
                  (web_idl.CallbackFunction, web_idl.CallbackInterface)):
        return name_style.class_("v8", idl_definition.identifier)
    else:
        return name_style.class_(idl_definition.identifier)


def v8_bridge_class_name(idl_definition):
    """
    Returns the name of V8-from/to-Blink bridge class.
    """
    assert isinstance(idl_definition, (web_idl.Namespace, web_idl.Interface))

    return name_style.class_("v8", idl_definition.identifier)


def blink_type_info(idl_type):
    """
    Returns the types of Blink implementation corresponding to the given IDL
    type.  The returned object has the following attributes.

      member_t: The type of a member variable.  E.g. T => Member<T>
      ref_t: The type of a local variable that references to an already-existing
          value.  E.g. String => String&
      value_t: The type of a variable that behaves as a value.  E.g. String =>
          String
      is_nullable: True if the Blink implementation type can represent IDL null
          value by itself.
    """
    assert isinstance(idl_type, web_idl.IdlType)

    class TypeInfo(object):
        def __init__(self,
                     typename,
                     member_fmt="{}",
                     ref_fmt="{}",
                     value_fmt="{}",
                     is_nullable=False):
            self.member_t = member_fmt.format(typename)
            self.ref_t = ref_fmt.format(typename)
            self.value_t = value_fmt.format(typename)
            # Whether Blink impl type can represent IDL null or not.
            self.is_nullable = is_nullable

    real_type = idl_type.unwrap(typedef=True)

    if real_type.is_boolean or real_type.is_numeric:
        cxx_type = {
            "boolean": "bool",
            "byte": "int8_t",
            "octet": "uint8_t",
            "short": "int16_t",
            "unsigned short": "uint16_t",
            "long": "int32_t",
            "unsigned long": "uint32_t",
            "long long": "int64_t",
            "unsigned long long": "uint64_t",
            "float": "float",
            "unrestricted float": "float",
            "double": "double",
            "unrestricted double": "double",
        }
        return TypeInfo(cxx_type[real_type.keyword_typename])

    if real_type.is_string:
        return TypeInfo("String", ref_fmt="{}&", is_nullable=True)

    if real_type.is_buffer_source_type:
        return TypeInfo(
            'DOM{}'.format(real_type.keyword_typename),
            member_fmt="Member<{}>",
            ref_fmt="{}*",
            value_fmt="{}*",
            is_nullable=True)

    if real_type.is_symbol:
        assert False, "Blink does not support/accept IDL symbol type."

    if real_type.is_any or real_type.is_object:
        return TypeInfo("ScriptValue", ref_fmt="{}&", is_nullable=True)

    if real_type.is_void:
        assert False, "Blink does not support/accept IDL void type."

    if real_type.type_definition_object is not None:
        type_def_obj = real_type.type_definition_object
        blink_impl_type = (
            type_def_obj.code_generator_info.receiver_implemented_as
            or name_style.class_(type_def_obj.identifier))
        return TypeInfo(
            blink_impl_type,
            member_fmt="Member<{}>",
            ref_fmt="{}*",
            value_fmt="{}*",
            is_nullable=True)

    if (real_type.is_sequence or real_type.is_frozen_array
            or real_type.is_variadic):
        element_type = blink_type_info(real_type.element_type)
        return TypeInfo(
            "VectorOf<{}>".format(element_type.value_t), ref_fmt="{}&")

    if real_type.is_record:
        key_type = blink_type_info(real_type.key_type)
        value_type = blink_type_info(real_type.value_type)
        return TypeInfo(
            "VectorOfPairs<{}, {}>".format(key_type.value_t,
                                           value_type.value_t),
            ref_fmt="{}&")

    if real_type.is_promise:
        return TypeInfo("ScriptPromise", ref_fmt="{}&")

    if real_type.is_union:
        return TypeInfo("ToBeImplementedUnion")

    if real_type.is_nullable:
        inner_type = blink_type_info(real_type.inner_type)
        if inner_type.is_nullable:
            return inner_type
        return TypeInfo(
            "base::Optional<{}>".format(inner_type.value_t), ref_fmt="{}&")

    assert False, "Unknown type: {}".format(idl_type.syntactic_form)


def native_value_tag(idl_type):
    """Returns the tag type of NativeValueTraits."""
    assert isinstance(idl_type, web_idl.IdlType)

    real_type = idl_type.unwrap(typedef=True)

    if (real_type.is_boolean or real_type.is_numeric or real_type.is_string
            or real_type.is_any or real_type.is_object):
        return "IDL{}".format(real_type.type_name)

    if real_type.is_symbol:
        assert False, "Blink does not support/accept IDL symbol type."

    if real_type.is_void:
        assert False, "Blink does not support/accept IDL void type."

    if real_type.type_definition_object is not None:
        return blink_type_info(real_type).value_t

    if real_type.is_sequence:
        return "IDLSequence<{}>".format(
            native_value_tag(real_type.element_type))

    if real_type.is_record:
        return "IDLRecord<{}, {}>".format(
            native_value_tag(real_type.key_type),
            native_value_tag(real_type.value_type))

    if real_type.is_promise:
        return "IDLPromise"

    if real_type.is_union:
        return blink_type_info(real_type).value_t

    if real_type.is_nullable:
        return "IDLNullable<{}>".format(native_value_tag(real_type.inner_type))


def make_default_value_expr(idl_type, default_value):
    """
    Returns a set of C++ expressions to be used for initialization with default
    values.  The returned object has the following attributes.

      initializer: Used as "Type var(|initializer|);".  This is None if
          "Type var;" sets an appropriate default value.
      assignment_value: Used as "var = |assignment_value|;".
    """
    assert default_value.is_type_compatible_with(idl_type)

    if idl_type.is_union:
        for member_type in idl_type.flattened_member_types:
            if default_value.is_type_compatible_with(member_type):
                idl_type = member_type
                break
        assert False
    type_info = blink_type_info(idl_type)

    is_initializer_lightweight = False
    if default_value.idl_type.is_nullable:
        if idl_type.unwrap().type_definition_object is not None:
            initializer = "nullptr"
            is_initializer_lightweight = True
            assignment_value = "nullptr"
        elif idl_type.unwrap().is_string:
            initializer = None  # String::IsNull() by default
            assignment_value = "String()"
        elif type_info.value_t == "ScriptValue":
            initializer = None  # ScriptValue::IsEmpty() by default
            assignment_value = "ScriptValue()"
        else:
            assert not type_info.is_nullable
            initializer = None  # !base::Optional::has_value() by default
            assignment_value = "base::nullopt"
    elif default_value.idl_type.is_sequence:
        initializer = None  # VectorOf<T>::size() == 0 by default
        assignment_value = "{}()".format(type_info.value_t)
    elif default_value.idl_type.is_object:
        dict_name = blink_class_name(idl_type.unwrap().type_definition_object)
        value = _format("{}::Create()", dict_name)
        initializer = value
        assignment_value = value
    elif default_value.idl_type.is_boolean:
        value = "true" if default_value.value else "false"
        initializer = value
        is_initializer_lightweight = True
        assignment_value = value
    elif default_value.idl_type.is_integer:
        initializer = default_value.literal
        is_initializer_lightweight = True
        assignment_value = default_value.literal
    elif default_value.idl_type.is_floating_point_numeric:
        if default_value.value == float("NaN"):
            value_fmt = "std::numeric_limits<{type}>::quiet_NaN()"
        elif default_value.value == float("Infinity"):
            value_fmt = "std::numeric_limits<{type}>::infinity()"
        elif default_value.value == float("-Infinity"):
            value_fmt = "-std::numeric_limits<{type}>::infinity()"
        else:
            value_fmt = "{value}"
        value = value_fmt.format(
            type=type_info.value_t, value=default_value.literal)
        initializer = value
        is_initializer_lightweight = True
        assignment_value = value
    elif default_value.idl_type.is_string:
        value = "\"{}\"".format(default_value.value)
        initializer = value
        assignment_value = value
    else:
        assert False

    class DefaultValueExpr:
        def __init__(self, initializer, is_initializer_lightweight,
                     assignment_value):
            assert initializer is None or isinstance(initializer, str)
            assert isinstance(is_initializer_lightweight, bool)
            assert isinstance(assignment_value, str)
            self.initializer = initializer
            self.is_initializer_lightweight = is_initializer_lightweight
            self.assignment_value = assignment_value

    return DefaultValueExpr(
        initializer=initializer,
        is_initializer_lightweight=is_initializer_lightweight,
        assignment_value=assignment_value)


def make_v8_to_blink_value(blink_var_name,
                           v8_value_expr,
                           idl_type,
                           default_value=None):
    """
    Returns a SymbolNode whose definition converts a v8::Value to a Blink value.
    """
    assert isinstance(blink_var_name, str)
    assert isinstance(v8_value_expr, str)
    assert isinstance(idl_type, web_idl.IdlType)
    assert (default_value is None
            or isinstance(default_value, web_idl.LiteralConstant))

    T = TextNode
    F = lambda *args, **kwargs: T(_format(*args, **kwargs))

    def create_definition(symbol_node):
        blink_value_expr = _format(
            "NativeValueTraits<{_1}>::NativeValue({_2})",
            _1=native_value_tag(idl_type),
            _2=", ".join(["${isolate}", v8_value_expr, "${exception_state}"]))

        if default_value is None:
            return SymbolDefinitionNode(symbol_node, [
                F("const auto& ${{{}}} = {};", blink_var_name,
                  blink_value_expr),
                CxxUnlikelyIfNode(
                    cond="${exception_state}.HadException()",
                    body=T("return;")),
            ])

        nodes = []
        type_info = blink_type_info(idl_type)
        default_expr = make_default_value_expr(idl_type, default_value)
        if default_expr.initializer is None:
            nodes.append(F("{} ${{{}}};", type_info.value_t, blink_var_name))
        elif default_expr.is_initializer_lightweight:
            nodes.append(
                F("{} ${{{}}} = {};", type_info.value_t, blink_var_name,
                  default_expr.initializer))
        assignment = [
            F("${{{}}} = {};", blink_var_name, blink_value_expr),
            CxxUnlikelyIfNode(
                cond="${exception_state}.HadException()", body=T("return;")),
        ]
        if (default_expr.initializer is None
                or default_expr.is_initializer_lightweight):
            nodes.append(
                CxxLikelyIfNode(
                    cond="!{}->IsUndefined()".format(v8_value_expr),
                    body=assignment))
        else:
            nodes.append(
                CxxIfElseNode(
                    cond="{}->IsUndefined()".format(v8_value_expr),
                    then=F("${{{}}} = {};", blink_var_name,
                           default_expr.assignment_value),
                    then_likeliness=Likeliness.LIKELY,
                    else_=assignment,
                    else_likeliness=Likeliness.LIKELY))
        return SymbolDefinitionNode(symbol_node, nodes)

    return SymbolNode(blink_var_name, definition_constructor=create_definition)


def make_v8_to_blink_value_variadic(blink_var_name, v8_array,
                                    v8_array_start_index, idl_type):
    """
    Returns a SymbolNode whose definition converts an array of v8::Value
    (variadic arguments) to a Blink value.
    """
    assert isinstance(blink_var_name, str)
    assert isinstance(v8_array, str)
    assert isinstance(v8_array_start_index, (int, long))
    assert isinstance(idl_type, web_idl.IdlType)

    pattern = "const auto& ${{{_1}}} = ToImplArguments<{_2}>({_3});"
    _1 = blink_var_name
    _2 = native_value_tag(idl_type.element_type)
    _3 = [v8_array, str(v8_array_start_index), "${exception_state}"]
    text = _format(pattern, _1=_1, _2=_2, _3=", ".join(_3))

    def create_definition(symbol_node):
        return SymbolDefinitionNode(symbol_node, [
            TextNode(text),
            CxxUnlikelyIfNode(
                cond="${exception_state}.HadException()",
                body=TextNode("return;")),
        ])

    return SymbolNode(blink_var_name, definition_constructor=create_definition)
