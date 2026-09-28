"""CPU-only regression tests for staged UE import; no Unreal/GPU needed."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_ue_assets as ue
import install_ue_assets as installer
import ue_asset_policy as policy
from ue_assembly_format import parse_struct
from assemble_tree_crown import add_crown


class UEImportTests(unittest.TestCase):
    def test_assembly_struct_parser_handles_empty_arrays_and_transforms(self):
        self.assertEqual(parse_struct('(Parts=,Nodes=)'), {'Parts': [], 'Nodes': []})
        parsed = parse_struct('(Parts=((MeshObjectPath="/Game/Leaf",MaterialRemap=(1,0))),Nodes=((PartIndex=0,Transform=(Translation=(X=100,Y=200,Z=300)))))')
        self.assertEqual(parsed['Parts'][0]['MaterialRemap'], [1, 0])
        self.assertEqual(parsed['Nodes'][0]['Transform']['Translation']['Y'], 200)

    def test_crown_budget_materials_and_ue_coordinate_transform(self):
        v = np.zeros((4,12), dtype='<f4')
        v[:,:3] = [[-.5,0,0],[.5,0,0],[.5,0,1],[-.5,0,1]]
        v[:,4] = -1; v[:,8:11] = 1
        ind = np.array([0,1,2,0,2,3], dtype='<u4')
        images = [np.full((32,32,4), 255, dtype=np.uint8)]
        def layer(key, colour, normal):
            images.append(np.asarray(colour)); return len(images)-1
        row = {'role':'CommonTree_1','assembly': {'parts':[{'asset':'leaf','material_remap':[0]}], 'nodes':[
            {'PartIndex':0,'Transform':{'Translation':{'X':100,'Y':200,'Z':300}}},
            {'PartIndex':0,'Transform':{'Translation':{'X':200,'Y':200,'Z':300}}}]}}
        book = {'leaf':{'mesh':'leaf.ums','nodes':[]}}
        with patch.object(ue.scene, 'SIZE', 32):
            out, faces, leaves, stats = add_crown(row,book,Path('.'),v,ind,[0],images,layer,
                lambda path:(v.copy(),ind.copy()),ue.scene,8)
        self.assertEqual(len(faces)//3,8)
        self.assertEqual(leaves,6)
        self.assertEqual(stats['retained_nodes'],1)
        np.testing.assert_allclose(out[4:,:3].mean(axis=0),[1,-2,3.5],atol=1e-5)
        self.assertTrue(np.all(out[4:,11]==1))

    def test_prepared_role_budgets(self):
        for role, limit in policy.TRIANGLE_LIMITS.items():
            policy.check_triangles(role, limit)
            with self.assertRaises(ValueError): policy.check_triangles(role, limit + 1)
        for role, count in zip(ue.ROLES + ["Grass_1"], [9256, 8961, 408, 2500, 300, 1999, 1498, 600, 780]):
            policy.check_triangles(role, count)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "original.ums"
            path.write_bytes(struct.pack("<4sII", b"UMS1", 1000000, 3000000))
            with self.assertRaisesRegex(ValueError, "beyond-budget"):
                ue.load_mesh(path, "CommonTree_1")

    def test_only_consumed_material_channels_are_transferred(self):
        material = {"asset": "/test/material", "textures": {
            "Albedo": "colour.png", "Normal": "normal.png", "WindNoise": "huge-unused.png",
            "WinterAtlas": "unused.png", "Opacity": "None"}}
        self.assertEqual(policy.selected_textures(material), {"albedo": "colour.png", "normal": "normal.png"})

    def test_tree_cannot_silently_drop_leaf_material(self):
        materials = [{"scalars": {"IsLeaves": "0"}}, {"scalars": {"IsLeaves": "1"}}]
        policy.check_leaf_materials("CommonTree_1", materials, {0, 1})
        with self.assertRaisesRegex(ValueError, "foliage material lost"):
            policy.check_leaf_materials("CommonTree_1", materials, {0})
        with self.assertRaises(ValueError):
            policy.check_leaf_materials("Pine_1", materials[:1], {0})

    def test_source_texture_limits_without_decoding(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "texture.png"
            Image.new("RGB", (8, 8)).save(path)
            policy.check_png(path)
            raw = bytearray(path.read_bytes())
            struct.pack_into(">II", raw, 16, 4096, 4096)
            path.write_bytes(raw)
            with self.assertRaises(ValueError): policy.check_png(path)
            struct.pack_into(">II", raw, 16, 8, 8)
            raw[24] = 16
            path.write_bytes(raw)
            with self.assertRaises(ValueError): policy.check_png(path)

    def test_export_size_budget_rejects_before_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "small").write_bytes(b"12345")
            with patch.object(policy, "MAX_EXPORT_BYTES", 8):
                policy.check_export_bytes(root, 3)
                with self.assertRaises(ValueError): policy.check_export_bytes(root, 4)

    def test_normal_encoding(self):
        image = Image.new("RGB", (1, 1), (90, 20, 240))
        self.assertEqual(ue.normal_gl(image).getpixel((0, 0)), (90, 235, 240))

    def test_packed_normal_ignores_ao_translucency_channel(self):
        low = ue.packed_normal_gl(Image.new("RGB", (1, 1), (128, 128, 0)))
        high = ue.packed_normal_gl(Image.new("RGB", (1, 1), (128, 128, 255)))
        self.assertEqual(low.tobytes(), high.tobytes())
        self.assertEqual(low.getpixel((0, 0))[2], 255)

    def test_light_foliage_tints_and_transparency(self):
        material = {"graph": {"nodes": {"base": {"class": "MaterialExpressionConstant3Vector", "constant": "{r: 0, g: 0, b: 0, a: 0}"}}},
                    "vectors": {"Tint_2": [1, 0, 0, 0], "Tint_3": [0, 1, 0, 0], "Tint_1": [0, 0, 1, 0]},
                    "switches": {"is Shrub": True}}
        for rgb in ((255, 0, 0), (0, 255, 0), (0, 0, 255)):
            image = ue.light_foliage_colour(material, Image.new("RGB", (1, 1), rgb))
            self.assertEqual(image.getpixel((0, 0)), (*rgb, 255))
        image = ue.light_foliage_colour(material, Image.new("RGB", (1, 1), (0, 0, 0)))
        self.assertEqual(image.getpixel((0, 0))[3], 0)

    def test_mwam_does_not_export_unused_opacity_placeholder(self):
        material = {"asset": "/Game/MWLandscapeAutoMaterial/Materials/Plants/grass", "textures": {
            "MW_TextureBaseColor": "colour", "MW_TextureNormal": "normal", "MW_TextureOpacity": "unused"},
                    "switches": {"MW_[x]_UseSeparateOpacityTexture": False}}
        self.assertNotIn("opacity", policy.selected_textures(material))
        material["switches"]["MW_[x]_UseSeparateOpacityTexture"] = True
        self.assertEqual(policy.selected_textures(material)["opacity"], "unused")

    def test_ambiguous_material_is_not_guessed(self):
        material = {"asset": "/test/material", "files": {"Albedo": "a.png", "Diffuse": "b.png"}}
        with self.assertRaises(ValueError): ue.binding(material, "albedo")
        material["bindings"] = {"albedo": "Diffuse"}
        self.assertEqual(ue.binding(material, "albedo"), "b.png")
        with self.assertRaises(ValueError): ue.binding({"asset": "x", "files": {}}, "albedo")

    def test_truncated_mesh_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.ums"
            path.write_bytes(struct.pack("<4sII", b"UMS1", 3, 3))
            with self.assertRaises(ValueError): ue.load_mesh(path)

    def test_source_path_cannot_escape_export(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "source").mkdir()
            (root / "outside.png").touch()
            with self.assertRaises(ValueError): ue.source_file(root / "source", "../outside.png")

    def test_complete_stage_and_failed_stage_do_not_replace_live_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            Image.new("RGBA", (8, 8), (80, 150, 60, 255)).save(source / "colour.png")
            Image.new("RGB", (8, 8), (128, 100, 250)).save(source / "normal.png")
            vertices = np.array([[-.5, -.5, 0], [.5, -.5, 0], [0, .5, 0], [0, 0, 1]], dtype="<f4")
            data = np.zeros((4, 12), dtype="<f4")
            data[:, :3] = vertices
            data[:, 5] = 1
            data[:, 8:11] = 1
            indices = np.array([0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3], dtype="<u4")
            (source / "model.ums").write_bytes(struct.pack("<4sII", b"UMS1", 4, 12) + data.tobytes() + indices.tobytes())
            material = {"asset": "/test/material", "files": {"Albedo": "colour.png", "Normal": "normal.png"}}
            document = {"version": 1, "license": "Fixture only, no external assets",
                        "geometry_source": "catalogue_verified_cut_lod0",
                        "section_materials_verified": True,
                        "models": [{"role": role, "asset": "/test/" + role, "mesh": "model.ums",
                                    "materials": [dict(material, scalars={"IsLeaves": "1" if role in ue.ROLES[:2] else "0"})]}
                                   for role in ue.ROLES + ["Grass_1"]],
                        "terrain": {name: {"albedo": {"file": "colour.png"}, "normal": {"file": "normal.png"}}
                                    for name, _, _ in ue.TERRAIN}}
            (source / "export.json").write_text(json.dumps(document))
            output = root / "stage"
            unchecked = dict(document, geometry_source="hi_res")
            (source / "export.json").write_text(json.dumps(unchecked))
            with self.assertRaises(ValueError): ue.prepare(source, output)
            self.assertFalse(output.exists())
            (source / "export.json").write_text(json.dumps(document))
            with patch.object(ue.scene, "SIZE", 32): ue.prepare(source, output)
            manifest = json.loads((output / "scene_models/manifest.json").read_text())
            self.assertEqual([row["name"] for row in manifest["models"]], ue.ROLES)
            self.assertEqual(len(manifest["grass"]), 6)
            self.assertEqual(len(manifest["groves"]), 2)
            for row in manifest["models"]:
                self.assertEqual((output / "scene_models" / row["mesh"]).read_bytes()[:4], b"SCM2")
            for name in manifest["colours"] + manifest["normals"] + manifest["grass"]:
                self.assertTrue((output / "scene_models" / name).is_file())
            self.assertTrue((output / "terrain/grass/lush/grass_lush_normal@16.png").is_file())
            sentinel = output / "keep.txt"
            sentinel.write_text("live")
            with self.assertRaises(ValueError): ue.prepare(source, output)
            self.assertEqual(sentinel.read_text(), "live")
            # Minimal valid source-indexed SCC6, matching the tetrahedron L0.
            for row in manifest["models"]:
                raw = (output / "scene_models" / row["mesh"]).read_bytes()
                _, nv, nl = struct.unpack_from("<4sII", raw)
                header = struct.pack("<4s13I2f", b"SCC6", nv, 4, 0, 1, 0, 12, 1, 0, 1, 0, 0, 0, 0, 0., 0.)
                points = np.frombuffer(raw, dtype="<f4", count=nv * 12, offset=12 + 4 * nl).reshape(nv, 12)
                payload = indices.tobytes() + points[indices, :3].astype("<f4").tobytes()
                payload += struct.pack("<II", 0, 0)
                payload += struct.pack("<II6fIII", 0, 12, 0., 0., .5, 2., 0., float("inf"), 0, 0xffffffff, 0xffffffff)
                (output / "scene_models" / Path(row["mesh"]).with_suffix(".clusters")).write_bytes(header + payload)
            assets = root / "assets"
            (assets / "generated/scene_models").mkdir(parents=True)
            (assets / "generated/scene_models/old.txt").write_text("old resources")
            backup = installer.install(output, assets)
            self.assertTrue((assets / "generated/scene_models/.ue-imported").is_file())
            self.assertEqual((backup / "generated/scene_models/old.txt").read_text(), "old resources")
            live_manifest = (assets / "generated/scene_models/manifest.json").read_bytes()
            rename = Path.rename
            def fail_second_copy(path, target):
                if path.name == "1" and path.parent.name.startswith(".ue-install-"):
                    raise OSError("simulated installation failure")
                return rename(path, target)
            with patch.object(Path, "rename", fail_second_copy):
                with self.assertRaises(OSError): installer.install(output, assets)
            self.assertEqual((assets / "generated/scene_models/manifest.json").read_bytes(), live_manifest)
            self.assertTrue((assets / "terrain/grass/lush/grass_lush_albedo.png").is_file())
            (output / "scene_models/CommonTree_1.clusters").write_bytes(b"SCC6")
            with self.assertRaises(ValueError): installer.install(output, assets)
            self.assertTrue((assets / "generated/scene_models/.ue-imported").is_file())
            (source / "normal.png").unlink()
            broken = root / "broken"
            with patch.object(ue.scene, "SIZE", 32):
                with self.assertRaises(ValueError): ue.prepare(source, broken)
            self.assertFalse(broken.exists())


if __name__ == "__main__":
    unittest.main()

