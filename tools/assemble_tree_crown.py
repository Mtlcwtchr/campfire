"""Bake bounded crossed-card branch prototypes at authored Nanite assembly transforms.
Only prototypes are rasterized; never expand all source leaf triangles into a tree.
"""
import math
import numpy as np
from PIL import Image


def rotation(transform):
    q = transform.get('Rotation', {})
    x,y,z,w = (float(q.get(k, 1. if k=='W' else 0.)) for k in 'XYZW')
    norm = math.sqrt(x*x+y*y+z*z+w*w)
    if not math.isfinite(norm) or norm <= 1e-12: raise ValueError('Invalid assembly quaternion')
    x,y,z,w = x/norm,y/norm,z/norm,w/norm
    r = np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                  [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                  [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
    mirror=np.diag([1.,-1.,1.])
    return mirror @ r @ mirror


def add_crown(row, book, source, body, body_indices, root_layers, images, add_layer, load_mesh, scene, budget):
    assembly = row['assembly']
    nodes = assembly['nodes']
    if not nodes: raise ValueError('Tree has no crown assembly nodes')
    prototypes = {}
    half = scene.SIZE//2
    for part in assembly['parts']:
        asset = part['asset']
        record = book[asset]
        if record.get('nodes'):
            raise ValueError('Nested assembly requires explicit flattening: '+asset)
        mesh_path = (source / record['mesh']).resolve()
        if not mesh_path.is_relative_to(source.resolve()): raise ValueError('Unsafe assembly mesh path')
        v, ind = load_mesh(mesh_path)
        remap = part['material_remap'] or list(range(len(root_layers)))
        local_material = v[:,11].astype(int)
        if np.any(local_material<0) or np.any(local_material>=len(remap)) or any(i<0 or i>=len(root_layers) for i in remap):
            raise ValueError('Invalid assembly material map')
        v[:,11]=np.asarray(root_layers)[np.asarray(remap)[local_material]]
        low,high=v[:,:3].min(axis=0),v[:,:3].max(axis=0)
        centre=(low+high)*.5
        colour=Image.new('RGBA',(scene.SIZE,scene.SIZE))
        normal=Image.new('RGBA',(scene.SIZE,scene.SIZE),(128,128,255,255))
        cards=[]
        # Horizontal, depth, vertical axes. Three orthogonal cards per branch.
        for plane,(horizontal,depth,vertical) in enumerate(((1,0,2),(0,1,2),(0,2,1))):
            width=max(float(high[horizontal]-low[horizontal])*1.04,.001)
            height=max(float(high[vertical]-low[vertical])*1.04,.001)
            projected=v.copy()
            projected[:,:3]=v[:,[horizontal,depth,vertical]]
            projected[:,0]-=centre[horizontal]
            bottom=float(centre[vertical])-height*.5
            projected[:,2]-=bottom
            projected[:,3:6]=v[:,[horizontal+3,depth+3,vertical+3]]
            tile,_=scene.rasterize(projected,ind,images,width,height,0)
            ox,oy=(plane%2)*half,(plane//2)*half
            colour.paste(tile.resize((half,half),Image.Resampling.LANCZOS),(ox,oy))
            for hu,hv in ((0,0),(1,0),(1,1),(0,1)):
                point=centre.copy()
                point[horizontal]=centre[horizontal]+(hu-.5)*width
                point[vertical]=bottom+hv*height
                n=np.zeros(3);n[depth]=-1
                uv=((ox+.5+hu*(half-1))/scene.SIZE,(oy+.5+(1-hv)*(half-1))/scene.SIZE)
                cards.append([*point,*n,*uv,1,1,1,0])
        if not colour.getchannel('A').getbbox(): raise ValueError('Empty leaf prototype '+asset)
        layer=add_layer((row['role'],'assembly',asset),colour,normal)
        vertices=np.asarray(cards,dtype='<f4');vertices[:,11]=layer
        prototypes[asset]=(vertices,centre)
    available=(budget-len(body_indices)//3)//6
    if available <= 0: raise ValueError('No triangle budget left for crown')
    selected=np.linspace(0,len(nodes)-1,min(available,len(nodes)),dtype=int)
    grow=min(1.5,math.sqrt(len(nodes)/len(selected)))
    output=[body]; indices=[body_indices]; offset=len(body)
    face=np.array([0,1,2,0,2,3,4,5,6,4,6,7,8,9,10,8,10,11],dtype='<u4')
    for number in selected:
        node=nodes[int(number)]
        if node.get('TransformSpace','Local')!='Local': raise ValueError('Nonlocal assembly transform')
        part=assembly['parts'][int(node['PartIndex'])]
        template,centre=prototypes[part['asset']]
        v=template.copy(); t=node.get('Transform',{})
        scale=np.array([t.get('Scale3D',{}).get(k,1.) for k in 'XYZ'])
        if np.any(np.abs(scale)<1e-9): continue
        r=rotation(t)
        translation=np.array([t.get('Translation',{}).get(k,0.) for k in 'XYZ'])*[.01,-.01,.01]
        v[:,:3]=((v[:,:3]-centre)*grow+centre)*scale @ r.T+translation
        n=(v[:,3:6]/scale) @ r.T
        v[:,3:6]=n/np.maximum(np.linalg.norm(n,axis=1,keepdims=True),1e-8)
        output.append(v);indices.append(face+offset);offset+=len(v)
    combined=np.concatenate(output).astype('<f4'); faces=np.concatenate(indices).astype('<u4')
    if not np.isfinite(combined).all(): raise ValueError('Non-finite crown transform')
    crown_triangles=(len(faces)-len(body_indices))//3
    if not crown_triangles or len(faces)//3>budget: raise ValueError('Crown assembly budget/coverage failed')
    return combined,faces,crown_triangles,{'source_nodes':len(nodes),'retained_nodes':len(selected),'branch_growth':grow}

