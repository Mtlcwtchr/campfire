"""Discrete level-of-detail chains for the imported nature meshes.

A stylised tree from the source archive is six thousand triangles. Drawn at the
sixty projected pixels a forest actually shows, that is three triangles per
pixel: the rasteriser pays for a full quad per triangle, so most of the cost is
spent on geometry no one can see. The fix is retopology rather than a smaller
budget - the same silhouette carried by a few hundred triangles.

These meshes are two different things wearing one index buffer, and a single
simplifier ruins one of them:

- Solid geometry - trunks, branches, rocks, mushroom caps. Connected, closed,
  and exactly what a quadric-error edge collapse is for.
- Alpha-cut cards - the leaves. Nine hundred and sixty separate two-triangle
  quads in one tree, four hundred and fifty in a bush. Collapsing an edge of a
  quad leaves half a leaf hanging in the air; the only honest simplification is
  to draw fewer of them.

So solid parts collapse and cards are thinned. Thinning alone would strip a
canopy bare, so the survivors are scaled about their own centres to carry the
area the removed ones did. That is why a level can own a few vertices of its
own: the same leaf at a new size. Everything else indexes the original vertex
buffer, because collapses move a vertex onto an existing neighbour and never
onto a new optimal position, so the whole chain stays one vertex upload.

The error reported for a level is not the quadric cost, which is a weighted
plane residual and not a length. It is measured afterwards against the level's
real geometry: the distance from every original vertex to the simplified
surface, in model metres, as a maximum, a 95th percentile and an RMS. That is
what the renderer projects to pixels, so a level is chosen by how far the shape
may move on screen rather than by a triangle ratio guessed offline.
"""

from __future__ import annotations

import heapq
import math
from collections import defaultdict

import numpy as np

# How much a boundary or seam edge resists being collapsed, relative to the
# faces it borders. Large enough that an open silhouette outlives the interior,
# small enough that a small detail can still go once it is tiny on screen.
BOUNDARY_WEIGHT = 12.0
# A collapse that turns a face by more than this much is a fold, not a
# simplification, and is refused however cheap its quadric looks.
MAX_FOLD_COSINE = 0.2
# A component this small is a card, not a surface: there is no interior edge to
# collapse, and every edge of it is silhouette.
CARD_TRIANGLES = 2
# The fewest triangles a connected piece of solid geometry may be reduced to
# before it is left alone. Four is a closed tetrahedron: still a volume, still
# a silhouette, and no longer a branch that vanished between two levels.
COMPONENT_FLOOR = 4
# A surviving leaf may carry at most this much of the area of the ones that went
# with it. Past that the canopy stops reading as leaves and starts reading as
# tarpaulin, so a very coarse level accepts a thinner crown instead.
MAX_CARD_GROWTH = 2.0
WELD_DECIMALS = 5
VERTEX_FLOATS = 12
LAYER_COLUMN = 11


def _weld(positions):
    """Group attribute vertices that sit at the same point in space.

    The importer keeps glTF's split vertices: one per normal, UV and material
    combination. Collapsing those independently would tear the mesh apart along
    every seam, so topology is built on welded positions and the attribute
    vertices are carried along.
    """
    keys = np.round(positions.astype(np.float64), WELD_DECIMALS) + 0.0  # kill -0.0
    unique, inverse = np.unique(keys, axis=0, return_inverse=True)
    return unique, inverse.astype(np.int64).reshape(-1)


def _components(faces, count):
    """Connected components of a welded triangle list, as lists of face ids."""
    parent = list(range(count))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for trio in faces:
        root = find(int(trio[0]))
        for other in trio[1:]:
            parent[find(int(other))] = root
    groups = defaultdict(list)
    for face, trio in enumerate(faces):
        groups[find(int(trio[0]))].append(face)
    return list(groups.values())


def _face_planes(points, faces):
    a, b, c = points[faces[:, 0]], points[faces[:, 1]], points[faces[:, 2]]
    normals = np.cross(b - a, c - a)
    lengths = np.linalg.norm(normals, axis=1)
    good = lengths > 1e-18
    normals = np.where(good[:, None], normals / np.where(good, lengths, 1.0)[:, None], 0.0)
    offsets = -np.einsum('ij,ij->i', normals, a)
    return np.column_stack((normals, offsets)), lengths * 0.5


def _quadric_cost(quadric, point):
    v = np.array((point[0], point[1], point[2], 1.0))
    return max(0.0, float(v @ quadric @ v))


def _point_triangle_squared(points, a, b, c):
    """Squared distance from each point to its own triangle, all at once."""
    ab, ac = b - a, c - a
    ap = points - a
    d1 = np.einsum('ij,ij->i', ab, ap)
    d2 = np.einsum('ij,ij->i', ac, ap)
    bp = points - b
    d3 = np.einsum('ij,ij->i', ab, bp)
    d4 = np.einsum('ij,ij->i', ac, bp)
    cp = points - c
    d5 = np.einsum('ij,ij->i', ab, cp)
    d6 = np.einsum('ij,ij->i', ac, cp)
    vc = d1 * d4 - d3 * d2
    vb = d5 * d2 - d1 * d6
    va = d3 * d6 - d5 * d4
    total = va + vb + vc
    safe = np.where(np.abs(total) > 1e-30, total, 1.0)
    v = np.clip(vb / safe, 0.0, 1.0)
    w = np.clip(vc / safe, 0.0, 1.0)
    inside = (va >= 0) & (vb >= 0) & (vc >= 0) & (np.abs(total) > 1e-30)
    interior = a + v[:, None] * ab + w[:, None] * ac

    def on_edge(p, q):
        pq = q - p
        t = np.clip(np.einsum('ij,ij->i', points - p, pq) /
                    np.maximum(np.einsum('ij,ij->i', pq, pq), 1e-30), 0.0, 1.0)
        return p + t[:, None] * pq

    # Outside the triangle the barycentric answer is meaningless; the nearest
    # point is on one of the three edges, and the corners fall out of clamping.
    edges = np.stack((on_edge(a, b), on_edge(b, c), on_edge(c, a)))
    pick = np.argmin(np.linalg.norm(edges - points[None], axis=2), axis=0)
    closest = np.where(inside[:, None], interior, edges[pick, np.arange(len(points))])
    delta = closest - points
    return np.einsum('ij,ij->i', delta, delta)


def surface_error(points, positions, faces, budget=1 << 21):
    """Distance from `points` to the triangle soup: maximum, p95 and RMS metres.

    Every point against every triangle, in blocks sized so the intermediate
    arrays stay within `budget` point-triangle pairs rather than materialising a
    six-thousand by six-thousand grid.
    """
    if len(faces) == 0 or len(points) == 0:
        return float('inf'), float('inf'), float('inf')
    a, b, c = positions[faces[:, 0]], positions[faces[:, 1]], positions[faces[:, 2]]
    block = max(1, min(len(faces), budget // max(1, len(points))))
    rows = max(1, budget // block)
    best = np.full(len(points), np.inf)
    for first in range(0, len(points), rows):
        chunk = points[first:first + rows]
        near = np.full(len(chunk), np.inf)
        for start in range(0, len(faces), block):
            stop = min(start + block, len(faces))
            span = stop - start
            squared = _point_triangle_squared(
                np.repeat(chunk, span, axis=0),
                np.tile(a[start:stop], (len(chunk), 1)),
                np.tile(b[start:stop], (len(chunk), 1)),
                np.tile(c[start:stop], (len(chunk), 1)))
            near = np.minimum(near, squared.reshape(len(chunk), span).min(axis=1))
        best[first:first + rows] = near
    best = np.sqrt(np.maximum(best, 0.0))
    return (float(best.max()), float(np.percentile(best, 95)),
            float(math.sqrt(float((best * best).mean()))))


class _Solid:
    """Welded topology of the collapsible faces, plus their attribute corners."""

    def __init__(self, positions, welded, corners, layers, components):
        self.positions = positions
        self.welded = welded
        self.corners = corners.copy()               # attribute vertex per corner
        self.faces = welded[corners]                # welded vertex per corner
        self.layers = layers
        self.alive = np.ones(len(self.faces), dtype=bool)
        # A branch of a pine is its own closed component. Collapsing it away
        # costs almost nothing in plane residual and everything in silhouette,
        # so a component is allowed to become coarse but not to disappear.
        self.component = components
        self.live = defaultdict(int)
        for group in components:
            self.live[group] += 1
        self.whole = dict(self.live)
        self.floor = {group: min(count, COMPONENT_FLOOR)
                      for group, count in self.whole.items()}
        self.incident = defaultdict(set)
        for face, trio in enumerate(self.faces):
            for vertex in trio:
                self.incident[int(vertex)].add(face)
        # One layer per welded position holds for the whole imported catalogue,
        # and a collapse across two materials would smear one texture onto the
        # other, so the invariant is established rather than assumed.
        self.vertex_layer = {}
        for corner in self.corners.reshape(-1):
            vertex, layer = int(self.welded[corner]), int(self.layers[corner])
            if self.vertex_layer.setdefault(vertex, layer) != layer:
                self.vertex_layer[vertex] = -1
        self.quadrics = np.zeros((len(positions), 4, 4))
        self._build_quadrics()

    def _build_quadrics(self):
        if len(self.faces) == 0:
            return
        planes, areas = _face_planes(self.positions, self.faces)
        weights = np.maximum(areas, 1e-12)
        outer = planes[:, :, None] * planes[:, None, :] * weights[:, None, None]
        for corner in range(3):
            np.add.at(self.quadrics, self.faces[:, corner], outer)
        uses = defaultdict(list)
        for face, trio in enumerate(self.faces):
            for e in range(3):
                lo, hi = sorted((int(trio[e]), int(trio[(e + 1) % 3])))
                uses[(lo, hi)].append(face)
        seams = self._seam_vertices()
        # An edge on the open boundary, or one that separates two UV islands,
        # has no second face to hold it in place. Give it a plane of its own,
        # perpendicular to the surface, so the outline is the last thing to go.
        for (lo, hi), faces in uses.items():
            if len(faces) == 2 and lo not in seams and hi not in seams:
                continue
            edge = self.positions[hi] - self.positions[lo]
            length = float(np.linalg.norm(edge))
            if length < 1e-12:
                continue
            perpendicular = np.cross(edge / length, planes[faces[0], :3])
            size = float(np.linalg.norm(perpendicular))
            if size < 1e-9:
                continue
            perpendicular /= size
            plane = np.append(perpendicular, -float(perpendicular @ self.positions[lo]))
            outer = np.outer(plane, plane) * (BOUNDARY_WEIGHT * length * length)
            self.quadrics[lo] += outer
            self.quadrics[hi] += outer

    def _seam_vertices(self):
        seen = defaultdict(set)
        for corner in self.corners.reshape(-1):
            seen[int(self.welded[corner])].add(int(corner))
        return {vertex for vertex, group in seen.items() if len(group) > 1}

    def edges(self):
        found = set()
        for face, trio in enumerate(self.faces):
            if not self.alive[face]:
                continue
            for e in range(3):
                lo, hi = sorted((int(trio[e]), int(trio[(e + 1) % 3])))
                if lo != hi:
                    found.add((lo, hi))
        return found

    def triangles(self):
        return int(self.alive.sum())

    def live_corners(self):
        return self.corners[self.alive]

    def dissolves(self, source, target):
        """Would this collapse take a connected piece below its floor?"""
        dying = defaultdict(int)
        for face in self.incident[source]:
            if self.alive[face] and target in {int(v) for v in self.faces[face]}:
                dying[self.component[face]] += 1
        return any(self.live[group] - count < self.floor[group]
                   for group, count in dying.items())

    def spent(self, source, quota):
        """Has this vertex's own branch already given up its share?

        A single global queue reduces whatever is cheapest next, which on a pine
        means eighteen branches ground down to a tetrahedron while eighteen
        others keep every triangle they started with. The crown then reads as
        damaged rather than simplified, and the measured error is the error of
        the worst branch. A per-component quota spends the same fraction
        everywhere, and what the queue still decides is where inside a branch.
        """
        for face in self.incident[source]:
            if self.alive[face]:
                group = self.component[face]
                return self.live[group] <= quota.get(group, 0)
        return True

    def would_fold(self, source, target):
        """Does moving `source` onto `target` turn any surviving face over?"""
        for face in self.incident[source]:
            if not self.alive[face]:
                continue
            trio = [int(v) for v in self.faces[face]]
            if target in trio:
                continue                     # this face collapses away, not over
            moved = [target if v == source else v for v in trio]
            before = np.cross(self.positions[trio[1]] - self.positions[trio[0]],
                              self.positions[trio[2]] - self.positions[trio[0]])
            after = np.cross(self.positions[moved[1]] - self.positions[moved[0]],
                             self.positions[moved[2]] - self.positions[moved[0]])
            first, second = np.linalg.norm(before), np.linalg.norm(after)
            if first < 1e-16 or second < 1e-16:
                return True
            if float(before @ after) / (first * second) < MAX_FOLD_COSINE:
                return True
        return False

    def _replacement(self, source, target, face):
        """Which attribute vertex of `target` this face's corner becomes.

        Preferring one already used by a face on the collapsed edge keeps the
        UVs continuous across a seam; without it a corner can jump to the far
        side of a texture island and drag a swatch across the triangle.
        """
        candidates = defaultdict(int)
        for neighbour in self.incident[target]:
            if not self.alive[neighbour]:
                continue
            trio = [int(v) for v in self.faces[neighbour]]
            weight = 2 if source in trio else 1
            for corner in self.corners[neighbour]:
                if int(self.welded[corner]) == target:
                    candidates[int(corner)] += weight
        if candidates:
            return max(sorted(candidates), key=lambda c: candidates[c])
        for corner in self.corners[face]:
            if int(self.welded[corner]) == target:
                return int(corner)
        return None

    def collapse(self, source, target):
        for face in list(self.incident[source]):
            if not self.alive[face]:
                continue
            trio = [int(v) for v in self.faces[face]]
            if target in trio:
                self.alive[face] = False
                self.live[self.component[face]] -= 1
                continue
            replacement = self._replacement(source, target, face)
            if replacement is None:
                self.alive[face] = False
                self.live[self.component[face]] -= 1
                continue
            for corner in range(3):
                if trio[corner] == source:
                    self.faces[face, corner] = target
                    self.corners[face, corner] = replacement
            self.incident[target].add(face)
        self.incident[source] = set()
        self.quadrics[target] += self.quadrics[source]


class _Cards:
    """The flat alpha-cut quads, which are thinned rather than collapsed."""

    def __init__(self, vertices, positions, welded, corners, groups):
        self.vertices = vertices
        # A grown card stays inside the shape it came from. Letting one reach
        # past the silhouette would widen the billboard frame that every
        # azimuth shares, and the impostor would lose texels to empty margin
        # for the sake of geometry only the mid-distance levels ever draw.
        self.floor = float(vertices[:, 2].min())
        self.ceiling = float(vertices[:, 2].max())
        self.radius = float(np.linalg.norm(vertices[:, :2], axis=1).max())
        self.corners = [corners[group] for group in groups]
        self.centres, self.areas, self.order = [], [], []
        for index, faces in enumerate(self.corners):
            points = positions[welded[faces.reshape(-1)]]
            self.centres.append(points.mean(axis=0))
            _, area = _face_planes(positions, welded[faces])
            self.areas.append(float(area.sum()))
            # Thinning by size alone strips one side of a crown before the
            # other. A hash of the quantised centre spreads the survivors
            # through the canopy and keeps the choice identical between runs.
            key = tuple(np.round(self.centres[-1], 3))
            self.order.append((abs(hash(key)) % (1 << 32), index))
        self.order.sort()

    def __len__(self):
        return len(self.corners)

    def level(self, keep):
        """Indices and new vertices for `keep` cards, grown to hold the area.

        Indices into new vertices are relative: the caller knows where in the
        combined buffer they land. A level that needs no growth indexes the
        model's own vertices and adds nothing.
        """
        keep = max(0, min(len(self), keep))
        if keep == 0:
            return np.zeros((0, 3), dtype=np.int64), np.zeros((0, VERTEX_FLOATS)), 1.0
        chosen = sorted(index for _, index in self.order[:keep])
        kept = sum(self.areas[index] for index in chosen)
        growth = min(MAX_CARD_GROWTH,
                     math.sqrt(sum(self.areas) / kept) if kept > 0 else 1.0)
        if growth <= 1.0 + 1e-6:
            return np.concatenate([self.corners[index] for index in chosen]), \
                np.zeros((0, VERTEX_FLOATS)), 1.0
        faces, grown, remap = [], [], {}
        for index in chosen:
            group = self.corners[index]
            used = np.unique(group.reshape(-1))
            centre = self.vertices[used, :3].mean(axis=0)
            for corner in used:
                if int(corner) not in remap:
                    moved = self.vertices[int(corner)].copy()
                    moved[:3] = centre + (moved[:3] - centre) * growth
                    # The pivot sits on the ground, so a leaf near the base of a
                    # bush would grow straight through it. Better a slightly
                    # squashed card than one buried in the terrain.
                    moved[2] = min(max(moved[2], self.floor), self.ceiling)
                    reach = float(np.linalg.norm(moved[:2]))
                    if reach > self.radius > 0:
                        moved[:2] *= self.radius / reach
                    remap[int(corner)] = len(grown)
                    grown.append(moved)
            faces.append(np.vectorize(remap.__getitem__)(group))
        return np.concatenate(faces), np.array(grown), growth


def build_levels(vertices, indices, ratios):
    """Build a level chain for one model.

    Returns `(extra, levels)`: the vertices the chain adds after the model's own,
    and one entry per level with its index array into the combined buffer, its
    triangle count, its card count and its measured error in model metres.
    """
    vertices = np.asarray(vertices, dtype=np.float64)
    corners = np.asarray(indices, dtype=np.int64).reshape(-1, 3)
    positions, welded = _weld(vertices[:, :3])
    layers = vertices[:, LAYER_COLUMN].astype(np.int64)
    faces = welded[corners]
    groups = _components(faces, len(positions))
    card_groups = [group for group in groups if len(group) <= CARD_TRIANGLES]
    solid_groups = [group for group in groups if len(group) > CARD_TRIANGLES]
    solid_faces = np.concatenate(solid_groups or [[]]).astype(np.int64)
    owner = np.concatenate([np.full(len(group), index) for index, group
                            in enumerate(solid_groups)] or [[]]).astype(np.int64)
    solid = _Solid(positions, welded, corners[solid_faces], layers, owner)
    cards = _Cards(vertices, positions, welded, corners, card_groups)
    original = positions.copy()
    full = len(corners)

    # How far the furthest original point folded into a vertex has travelled.
    # Plane quadrics are area weighted, so a small isolated branch is cheap to
    # erase one collapse at a time even though the shape loses it entirely.
    # Charging the accumulated drift makes the second collapse in a region cost
    # more than the first, which is what keeps small features alive.
    drift = np.zeros(len(positions))

    def cost(a, b):
        combined = solid.quadrics[a] + solid.quadrics[b]
        span = float(np.linalg.norm(positions[a] - positions[b]))
        forward = _quadric_cost(combined, positions[b]) + (drift[a] + span) ** 2
        backward = _quadric_cost(combined, positions[a]) + (drift[b] + span) ** 2
        return (forward, a, b) if forward <= backward else (backward, b, a)

    heap = []
    for lo, hi in solid.edges():
        if solid.vertex_layer.get(lo, -1) != solid.vertex_layer.get(hi, -2):
            continue                      # never merge two materials
        heapq.heappush(heap, cost(lo, hi))

    def reduce_to(ratio):
        quota = {group: max(solid.floor[group], int(round(whole * ratio)))
                 for group, whole in solid.whole.items()}
        held = []
        while heap:
            value, source, onto = heapq.heappop(heap)
            if source == onto or not solid.incident[source] or not solid.incident[onto]:
                continue
            current = cost(source, onto)
            if current[0] > value + 1e-12:
                heapq.heappush(heap, current)      # stale: the region has changed
                continue
            if solid.spent(source, quota):
                held.append(current)   # this branch is done for now, not for good
                continue
            if solid.dissolves(source, onto) or solid.would_fold(source, onto):
                continue
            span = float(np.linalg.norm(positions[source] - positions[onto]))
            drift[onto] = max(drift[onto], drift[source] + span)
            solid.collapse(source, onto)
            for neighbour in {int(v) for face in solid.incident[onto]
                              if solid.alive[face] for v in solid.faces[face]} - {onto}:
                if solid.vertex_layer.get(neighbour, -1) != solid.vertex_layer.get(onto, -2):
                    continue
                heapq.heappush(heap, cost(onto, neighbour))
        for item in held:
            heapq.heappush(heap, item)

    extra = np.zeros((0, VERTEX_FLOATS))
    levels = []
    for ratio in sorted({r for r in ratios if 0 < r < 1}, reverse=True):
        reduce_to(ratio)
        card_faces, grown, growth = cards.level(int(round(len(cards) * ratio)))
        level = np.concatenate((solid.live_corners(), card_faces + len(vertices) + len(extra))) \
            if len(grown) else np.concatenate((solid.live_corners(), card_faces))
        # A level that saved nothing is not a level: the renderer would draw the
        # same triangles under a coarser name and choose it for a smaller size.
        if len(level) >= full or (levels and len(level) >= levels[-1]['triangles']):
            continue
        if len(grown):
            extra = np.concatenate((extra, grown))
        combined = np.concatenate((vertices, extra)) if len(extra) else vertices
        levels.append({
            'indices': level.reshape(-1).astype('<u4'),
            'triangles': int(len(level)),
            'cards': int(len(card_faces) // CARD_TRIANGLES),
            'card_growth': float(growth),
            'error': surface_error(original, combined[:, :3], level),
        })
    return extra, levels
