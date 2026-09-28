"""Read-only material graph diagnostics for CPU conversion of used UE materials."""
import unreal


def describe(material):
    lib = unreal.MaterialEditingLibrary
    base = material
    while isinstance(base, unreal.MaterialInstance):
        base = base.get_editor_property("parent")
    roots = {name: lib.get_material_property_input_node(base, prop) for name, prop in
             (("colour", unreal.MaterialProperty.MP_BASE_COLOR), ("opacity", unreal.MaterialProperty.MP_OPACITY_MASK))}
    result = {"material": base.get_path_name(), "roots": {k: v.get_name() if v else None for k, v in roots.items()}, "nodes": {}}
    stack = [v for v in roots.values() if v]
    while stack and len(result["nodes"]) < 128:
        node = stack.pop()
        name = node.get_name()
        if name in result["nodes"]:
            continue
        entry = {"class": node.get_class().get_name(), "inputs": [],
                 "input_names": list(lib.get_material_expression_input_names(node))}
        for field in ("parameter_name", "constant", "r", "g", "b", "a", "const_a", "const_b", "const_alpha", "default_value", "texture", "material_function"):
            try:
                value = node.get_editor_property(field)
                entry[field] = value.get_path_name() if isinstance(value, unreal.Object) else str(value)
            except Exception:
                pass
        for child in lib.get_inputs_for_material_expression(base, node):
            if child:
                entry["inputs"].append({"node": child.get_name(), "output": str(lib.get_input_node_output_name_for_material_expression(node, child))})
                stack.append(child)
        result["nodes"][name] = entry
    return result

