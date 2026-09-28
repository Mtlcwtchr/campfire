import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import numpy as np
from PIL import Image
import bake_impostor_depth as baker


class DepthAtlasTests(unittest.TestCase):
    def setUp(self):
        self.size = patch.object(baker.scene, 'SIZE', 16)
        self.size.start()
        self.addCleanup(self.size.stop)

    def catalogue(self, root):
        vertices = []
        for layer, y in enumerate((.5, -.5)):
            for x, z in ((-.8, .1), (.8, .1), (.8, 1.9), (-.8, 1.9)):
                vertices.append([x, y, z, 0, 1, 0, .5, .5, 1, 1, 1, layer])
        v = np.array(vertices, dtype='<f4')
        indices = np.array([0,1,2,0,2,3,4,5,6,4,6,7], dtype='<u4')
        (root/'model.mesh').write_bytes(struct.pack('<4sIII', b'SCM2', 8, 1, 12) + v.tobytes() + indices.tobytes())
        for i in range(10):
            Image.new('RGBA', (16,16), (255,0,0,255) if i==0 else (0,255,0,255)).save(root/f'c{i}.png')
            Image.new('RGBA', (16,16), (128,255,128,255)).save(root/f'n{i}.png')
        manifest = {'version':2,'views':8,'colours':[f'c{i}.png' for i in range(10)],
                    'normals':[f'n{i}.png' for i in range(10)],'groves':[],
                    'models':[{'name':'test','mesh':'model.mesh','width':2,'height':2,'impostor':2}]}
        (root/'manifest.json').write_text(json.dumps(manifest))
        return manifest

    def test_signed_depth_precision_and_empty_coverage(self):
        depth=np.array([[-2,-.5,0,.5,2,np.nan]])
        encoded=np.asarray(baker.encode_depth(depth,np.full(depth.shape,255),4,7))
        decoded=((encoded[...,0].astype(int)*256+encoded[...,1])/65535-.5)*4
        self.assertLessEqual(np.max(np.abs(decoded[0,:5]-depth[0,:5])),4/65535)
        self.assertEqual(encoded[0,-1,3],0)
        self.assertTrue(np.all(encoded[...,2]==7))

    def test_bake_colour_and_depth_share_the_front_surface(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);original=self.catalogue(root)
            source=(root/'model.mesh').read_bytes()
            baker.bake(root)
            result=json.loads((root/'manifest.json').read_text())
            self.assertEqual(result['version'],2)
            self.assertEqual(result['colours'][:2],original['colours'][:2])
            self.assertEqual((root/'model.mesh').read_bytes(),source)
            self.assertEqual(result['depth_atlas']['encoding'],baker.ENCODING)
            for view,channel in ((0,0),(4,1)):
                with Image.open(root/result['colours'][2+view]) as image:
                    colour=np.asarray(image)
                with Image.open(root/result['depth_atlas']['layers'][2+view]) as image:
                    depth=np.asarray(image)
                self.assertEqual(colour[8,8,channel],255)
                self.assertEqual(depth[8,8,3],colour[8,8,3])
                self.assertEqual(depth[8,8,2],view)
                value=((int(depth[8,8,0])*256+int(depth[8,8,1]))/65535-.5)*2
                self.assertAlmostEqual(value,.5,delta=2/65535)

    def test_failure_does_not_publish_partial_catalogue(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);manifest=self.catalogue(root)
            manifest['models'][0]['mesh']='../outside.mesh'
            path=root/'manifest.json';path.write_text(json.dumps(manifest));before=path.read_bytes()
            with self.assertRaises(ValueError): baker.bake(root)
            self.assertEqual(path.read_bytes(),before)
            self.assertEqual(list(root.glob('depth-*')),[])

    def test_rejects_truncated_mesh_and_out_of_range_depth(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'bad.mesh';path.write_bytes(b'SCM2')
            with self.assertRaises(ValueError): baker.read_mesh(path)
        with self.assertRaises(ValueError): baker.encode_depth(np.array([[3.]]),np.array([[255]]),4,0)

    def test_hemisphere_basis_is_orthonormal_with_fixed_top_view(self):
        for view in range(baker.hemi.VIEW_COUNT):
            b=baker.hemi.basis(view)
            np.testing.assert_allclose(b@b.T,np.eye(3),atol=1e-12)
            np.testing.assert_allclose(np.cross(b[0],b[1]),-b[2],atol=1e-12)
            self.assertGreaterEqual(b[2,2],0)
        np.testing.assert_allclose(baker.hemi.basis(20)[2],[0,0,1],atol=1e-12)

    def test_hemisphere_top_depth_and_repeat_bake_preserve_catalogue(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);original=self.catalogue(root)
            vertices=[]
            for layer,z in enumerate((.4,1.6)):
                for x,y in ((-.6,-.6),(.6,-.6),(.6,.6),(-.6,.6)):
                    vertices.append([x,y,z,0,0,1,.5,.5,1,1,1,layer])
            source=struct.pack('<4sIII',b'SCM2',8,1,12)+np.array(vertices,dtype='<f4').tobytes()+np.array(
                [0,1,2,0,2,3,4,5,6,4,6,7],dtype='<u4').tobytes()
            (root/'model.mesh').write_bytes(source)
            for _ in range(2):
                baker.bake(root,hemisphere=True)
                result=json.loads((root/'manifest.json').read_text())
                self.assertEqual(len(result['colours']),10+baker.hemi.VIEW_COUNT)
                self.assertEqual(result['colours'][:2],original['colours'][:2])
                self.assertEqual(result['models'][0]['impostor'],2)
                self.assertEqual((root/'model.mesh').read_bytes(),source)
                frame=result['models'][0]['hemisphere_impostor']
                self.assertEqual(frame['first'],10);self.assertEqual(frame['views'],21)
                self.assertEqual(frame['layout'],baker.hemi.LAYOUT)
                for view in range(21):
                    index=frame['first']+view
                    with Image.open(root/result['colours'][index]) as im: colour=np.asarray(im)
                    with Image.open(root/result['depth_atlas']['layers'][index]) as im: depth=np.asarray(im)
                    np.testing.assert_array_equal(colour[...,3],depth[...,3])
                    self.assertTrue(np.all(depth[...,2]==view))
                self.assertEqual(colour[8,8,1],255) # the green upper plane, not the red bottom
                value=((int(depth[8,8,0])*256+int(depth[8,8,1]))/65535-.5)*frame['side']
                self.assertAlmostEqual(value,.6,delta=frame['side']/65535)

    def test_hemisphere_range_failure_is_transactional(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);manifest=self.catalogue(root)
            manifest['models'][0]['hemisphere_impostor']={'layout':baker.hemi.LAYOUT,'views':21,'first':0}
            path=root/'manifest.json';path.write_text(json.dumps(manifest));before=path.read_bytes()
            with self.assertRaises(ValueError): baker.bake(root,hemisphere=True)
            self.assertEqual(path.read_bytes(),before);self.assertEqual(list(root.glob('depth-*')),[])


if __name__ == '__main__':
    unittest.main()
