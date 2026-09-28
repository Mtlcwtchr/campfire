"""Export small assembly parts + transforms, never high-resolution tree trunks."""
import json
from pathlib import Path
import struct
import unreal
from ue_assembly_format import parse_struct


def export_parts(out):
    gs, q, lists = unreal.GeometryScript_AssetUtils, unreal.GeometryScript_MeshQueries, unreal.GeometryScript_List
    from export_ue_assets import pick, vectors
    checkpoint = out / 'assembly-cache.json'
    cache = json.loads(checkpoint.read_text()) if checkpoint.exists() else {}
    def part(path):
        if path in cache: return cache[path]
        mesh = unreal.load_asset(path)
        if mesh is None: raise ValueError('Missing assembly part: ' + path)
        settings = mesh.get_editor_property('nanite_settings')
        assembly = parse_struct(settings.get_editor_property('nanite_assembly_data').export_text())
        index = len(cache)
        row = {'asset': path, 'mesh': f'assembly/part-{index}.ums', 'parts': [], 'nodes': assembly.get('Nodes', [])}
        cache[path] = row
        lod = unreal.GeometryScriptMeshReadLOD()
        lod.set_editor_property('lod_type', unreal.GeometryScriptLODType.SOURCE_MODEL)
        lod.set_editor_property('lod_index', 0)
        options = unreal.GeometryScriptCopyMeshFromAssetOptions()
        def read_lod():
            target = unreal.DynamicMesh()
            result = gs.copy_mesh_from_skeletal_mesh(mesh, target, options, lod) if isinstance(mesh, unreal.SkeletalMesh) else gs.copy_mesh_from_static_mesh_v2(mesh, target, options, lod, False)
            return pick(result, unreal.DynamicMesh)
        dm = read_lod()
        count = q.get_num_triangle_i_ds(dm)
        original_count = count
        if count > 8192 and isinstance(mesh, unreal.SkeletalMesh):
            editor = getattr(unreal, 'SkeletalMeshEditorSubsystem', None)
            lod_count = editor.get_lod_count(mesh) if editor else 1
            for level in range(1, lod_count):
                lod.set_editor_property('lod_index', level)
                candidate = read_lod()
                candidate_count = q.get_num_triangle_i_ds(candidate)
                if 0 < candidate_count < count:
                    dm, count = candidate, candidate_count
                if count <= 8192: break
        if count > 8192:
            lod.set_editor_property('lod_type', unreal.GeometryScriptLODType.RENDER_DATA)
            lod.set_editor_property('lod_index', 0)
            candidate = read_lod()
            candidate_count = q.get_num_triangle_i_ds(candidate)
            unreal.log_warning(f'AS_PART_RENDER {path} source={count} render={candidate_count}')
            if 0 < candidate_count < count:
                dm, count = candidate, candidate_count
        if count > 8192:
            if count > 2000000: raise ValueError('Assembly prototype exceeds processing guard: ' + path)
            simplify = unreal.GeometryScriptSimplifyMeshOptions()
            simplify.set_editor_property('preserve_vertex_positions', False)
            simplify.set_editor_property('method', unreal.GeometryScriptRemoveMeshSimplificationType.STANDARD_QEM)
            dm = unreal.GeometryScript_MeshSimplification.apply_simplify_to_triangle_count(dm, 8192, simplify)
            count = q.get_num_triangle_i_ds(dm)
        row['original_triangles'] = original_count
        triangles = lists.convert_triangle_list_to_array(pick(q.get_all_triangle_indices(dm, False), unreal.GeometryScriptTriangleList))
        live_count = sum(min(t.x,t.y,t.z) >= 0 for t in triangles)
        selected = None
        if live_count > 9800:
            # A QEM cannot remove whole disconnected leaves. Thin complete
            # components for this offline prototype bake; never tear faces out.
            parent = {}
            def find(v):
                parent.setdefault(v, v)
                while parent[v] != v:
                    parent[v] = parent[parent[v]]; v = parent[v]
                return v
            for t in triangles:
                if min(t.x,t.y,t.z) < 0: continue
                a,b,c = find(t.x),find(t.y),find(t.z)
                parent[b] = a; parent[c] = a
            groups = {}
            for tid,t in enumerate(triangles):
                if min(t.x,t.y,t.z) >= 0: groups.setdefault(find(t.x), []).append(tid)
            groups = sorted(groups.items(), key=lambda item: (-len(item[1]),item[0]))
            selected = set(groups[0][1])
            for root,members in sorted(groups[1:], key=lambda item: (item[0]*2654435761)&0xffffffff):
                if len(selected)+len(members) <= 9800: selected.update(members)
            row['components_before_thinning'] = len(groups)
            live_count = len(selected)
        unreal.log_warning(f'AS_PART_COUNTS {path} source={original_count} ids={count} live={live_count}')
        if not 0 < live_count <= 20000: raise ValueError('Assembly part could not fit transfer guard: ' + path)
        positions = lists.convert_vector_list_to_array(pick(q.get_all_vertex_positions(dm, False), unreal.GeometryScriptVectorList))
        data, indices, unique = bytearray(), [], {}
        materials = {}
        for tid, tri in enumerate(triangles):
            if min(tri.x, tri.y, tri.z) < 0: continue
            if selected is not None and tid not in selected: continue
            normals = vectors(q.get_triangle_normals(dm, tid), unreal.Vector)
            uv = vectors(q.get_triangle_u_vs(dm, 0, tid), unreal.Vector2D)
            material, valid = unreal.GeometryScript_Materials.get_triangle_material_id(dm, tid)
            if not valid: raise ValueError('Assembly material IDs absent')
            materials[material] = materials.get(material, 0) + 1
            for corner in (0, 2, 1):
                p = positions[(tri.x, tri.y, tri.z)[corner]]
                n, t = normals[corner], uv[corner]
                packed = struct.pack('<12f', p.x*.01, -p.y*.01, p.z*.01, n.x, -n.y, n.z, t.x, t.y, 1., 1., 1., float(material))
                if packed not in unique:
                    unique[packed] = len(unique); data.extend(packed)
                indices.append(unique[packed])
        dest = out / row['mesh']; dest.parent.mkdir(exist_ok=True)
        dest.write_bytes(struct.pack('<4sII', b'UMS1', len(unique), len(indices)) + data + struct.pack(f'<{len(indices)}I', *indices))
        row['triangles'] = len(indices)//3; row['material_triangles'] = materials
        for child in assembly.get('Parts', []):
            row['parts'].append({'asset': child['MeshObjectPath'], 'material_remap': child.get('MaterialRemap', [])})
            part(child['MeshObjectPath'])
        unreal.log_warning('AS_ASSEMBLY_PART ' + path + ' ' + str(row['material_triangles']))
        checkpoint.write_text(json.dumps({p:r for p,r in cache.items() if 'triangles' in r}, indent=1))
        return row
    roots = {}
    for role in ('CommonTree_1', 'Pine_1'):
        assembly = parse_struct((out / (role + '-assembly.txt')).read_text())
        roots[role] = {'nodes': assembly['Nodes'], 'parts': []}
        for child in assembly['Parts']:
            roots[role]['parts'].append({'asset': child['MeshObjectPath'], 'material_remap': child.get('MaterialRemap', [])})
            part(child['MeshObjectPath'])
    (out / 'assembly-parts.json').write_text(json.dumps({'roots': roots, 'parts': cache}, indent=1))
    unreal.log_warning('AS_ASSEMBLY parts complete')

