// Stand-in for collision.js (same interface): flat floor at y = -98.44, no walls.
// Game space, -Y up: "down" is +y.
const FLOOR_Y = -98.44;

export async function loadCollision(_url) {
  return {
    stub: true,
    groundY(_x, _z, feetY) {
      return FLOOR_Y >= feetY - 5 ? FLOOR_Y : null;
    },
    move(pos, delta, _opts = {}) {
      return { x: pos.x + delta.x, y: FLOOR_Y, z: pos.z + delta.z, onGround: true, hitWall: false };
    },
    segment(_a, _b) { return null; },
  };
}
