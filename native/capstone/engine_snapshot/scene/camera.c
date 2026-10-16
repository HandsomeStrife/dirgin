/* =====================================================================
 * camera.c  --  Implementation of the reauthored camera / view subsystem.
 *
 * Matrix layout note (the one convention bridge for this module):
 *   The guest builds its matrices ROW-major and transforms ROW vectors
 *   (v' = v * M), which is the PS2/VU convention. The native engine's
 *   dmat4 is COLUMN-major and transforms COLUMN vectors (v' = M * v).
 *   For the SAME linear map, the column-major storage of M (column
 *   convention) is byte-identical to the row-major storage of M (row
 *   convention): column-major[j*4+i] == row-major[i*4+j] of the transpose,
 *   and the transpose is exactly what flips v*M into M*v. Net result: the
 *   16 floats we store in a dmat4 are the SAME 16 floats the guest stores,
 *   in the same order, and they drive the identical transform under the
 *   renderer's column-vector convention. So each build below writes the
 *   guest's float indices directly into dmat4.m[], and the oracle-parity
 *   test asserts clean.m[k] == oracle_rowmajor[k] (no transpose needed) --
 *   which both validates the math and pins the index map.
 * ===================================================================== */
#include "camera.h"
#include <math.h>

/* ---------------------------------------------------------------------
 * State init + script-facing setters/getters (KerberosCamera bridge).
 * ------------------------------------------------------------------- */
void doc_camera_init(DocCamera *c) {
    c->eye      = dvec3_make(0.0f, 0.0f, 0.0f);
    c->angles   = dvec3_make(0.0f, 0.0f, 0.0f);
    c->order[0] = DOC_AXIS_X;   /* applied first (R = Rz*Ry*Rx) */
    c->order[1] = DOC_AXIS_Y;
    c->order[2] = DOC_AXIS_Z;
    c->fov      = 1.0471975512f;   /* 60 degrees default */
    c->screen_w = 640.0f;          /* NTSC DoC display extents */
    c->screen_h = 448.0f;
    c->proj_dist = (c->screen_h * 0.5f) / tanf(c->fov * 0.5f);
    c->znear    = 1.0f;            /* FUN_00263968 clamps near to >= 1 */
    c->zfar     = 4096.0f;
}

dvec3 doc_camera_get_position(const DocCamera *c) {
    return c->eye;              /* KerberosCamera.getpos */
}

void doc_camera_move(DocCamera *c, float dx, float dy, float dz) {
    c->eye.x += dx;             /* KerberosCamera.move(FFF) */
    c->eye.y += dy;
    c->eye.z += dz;
}

void doc_camera_set_fov(DocCamera *c, float fov_radians) {
    c->fov = fov_radians;       /* KerberosCamera.setfov(F) -> DAT_004fcacc */
    c->proj_dist = (c->screen_h * 0.5f) / tanf(c->fov * 0.5f);
}

void doc_camera_set_angles(DocCamera *c, dvec3 angles_radians) {
    c->angles = angles_radians;
}

void doc_camera_set(DocCamera *c, dvec3 eye, dvec3 target, dvec3 up) {
    /* KerberosCamera.set(eye,target,up): store eye, then derive the Euler
     * orientation of the camera-to-world basis that looks toward target.
     * We build an orthonormal basis (forward = target-eye) as a dmat4 and
     * run the same matrix->Euler extraction the guest uses (FUN_002d9cd0). */
    c->eye = eye;

    dvec3 fwd = dvec3_normalize(dvec3_sub(target, eye));
    dvec3 right = dvec3_normalize(dvec3_cross(up, fwd));
    dvec3 realup = dvec3_cross(fwd, right);

    /* camera-to-world rotation, column-major: columns = basis vectors.
     * (right, realup, fwd) as the camera's X,Y,Z axes in world space.) */
    dmat4 basis = dmat4_identity();
    basis.m[0] = right.x;  basis.m[1] = right.y;  basis.m[2] = right.z;
    basis.m[4] = realup.x; basis.m[5] = realup.y; basis.m[6] = realup.z;
    basis.m[8] = fwd.x;    basis.m[9] = fwd.y;    basis.m[10] = fwd.z;

    c->angles = doc_camera_matrix_to_angles(basis);
    c->order[0] = DOC_AXIS_X; c->order[1] = DOC_AXIS_Y; c->order[2] = DOC_AXIS_Z;
}

void doc_camera_get_vectors(const DocCamera *c, dvec3 out[3]) {
    /* KerberosCamera.getCurrentCameraVectors: eye, a target one unit along
     * the camera forward (+Z of the camera basis), and the camera up (+Y). */
    dmat4 R = doc_camera_view_rotation(c);   /* camera-to-world basis */
    dvec3 fwd = dvec3_make(R.m[8], R.m[9], R.m[10]);
    dvec3 up  = dvec3_make(R.m[4], R.m[5], R.m[6]);
    out[0] = c->eye;
    out[1] = dvec3_add(c->eye, fwd);
    out[2] = up;
}

/* ---------------------------------------------------------------------
 * Single-axis Euler rotation matrices.
 *   FUN_00259928 (X), FUN_00259990 (Y), FUN_002599f8 (Z).
 * Each guest build: 3x3 identity (FUN_002613e0) + the sin/cos entries,
 * with the homogeneous row/col = (0,0,0,1) from the surrounding identity.
 * We write the guest float indices straight into the dmat4 (see layout
 * note at top): c = cosf(angle), s = sinf(angle).
 * ------------------------------------------------------------------- */
dmat4 doc_camera_rot_x(float angle) {
    float c = cosf(angle), s = sinf(angle);
    dmat4 r = dmat4_identity();
    /* guest FUN_00259928: [5]=c [6]=s [9]=-s [10]=c */
    r.m[5] = c;  r.m[6] = s;  r.m[9] = -s;  r.m[10] = c;
    return r;
}

dmat4 doc_camera_rot_y(float angle) {
    float c = cosf(angle), s = sinf(angle);
    dmat4 r = dmat4_identity();
    /* guest FUN_00259990: [0]=c [2]=-s [8]=s [10]=c */
    r.m[0] = c;  r.m[2] = -s;  r.m[8] = s;  r.m[10] = c;
    return r;
}

dmat4 doc_camera_rot_z(float angle) {
    float c = cosf(angle), s = sinf(angle);
    dmat4 r = dmat4_identity();
    /* guest FUN_002599f8: [0]=c [1]=s [4]=-s [5]=c */
    r.m[0] = c;  r.m[1] = s;  r.m[4] = -s;  r.m[5] = c;
    return r;
}

dmat4 doc_camera_axis_rotation(DocAxis axis, float angle) {
    switch (axis) {
        case DOC_AXIS_X: return doc_camera_rot_x(angle);
        case DOC_AXIS_Y: return doc_camera_rot_y(angle);
        case DOC_AXIS_Z: return doc_camera_rot_z(angle);
    }
    return dmat4_identity();
}

/* FUN_00259c50: compose the three axis rotations in the camera's order.
 * The guest composes (row-major, row-vector) M = R[o0]*R[o1]*R[o2] via two
 * FUN_0026125c multiplies; the identical map in the renderer's column-
 * vector convention is dmat4_mul(R[o2], dmat4_mul(R[o1], R[o0])) (see the
 * layout note -- dmat4_mul(a,b) composes so that b acts first). The angle
 * for the rotation that uses axis order[i] is angles[order[i]]. */
static float doc_angle_for_axis(const DocCamera *c, DocAxis a) {
    switch (a) {
        case DOC_AXIS_X: return c->angles.x;
        case DOC_AXIS_Y: return c->angles.y;
        case DOC_AXIS_Z: return c->angles.z;
    }
    return 0.0f;
}

dmat4 doc_camera_view_rotation(const DocCamera *c) {
    dmat4 R0 = doc_camera_axis_rotation(c->order[0], doc_angle_for_axis(c, c->order[0]));
    dmat4 R1 = doc_camera_axis_rotation(c->order[1], doc_angle_for_axis(c, c->order[1]));
    dmat4 R2 = doc_camera_axis_rotation(c->order[2], doc_angle_for_axis(c, c->order[2]));
    return dmat4_mul(R2, dmat4_mul(R1, R0));
}

/* FUN_002610e0: rigid inverse of an orthonormal (rotation R + translation
 * t) camera-to-world transform -> world-to-camera. Inverse rotation = R^T,
 * inverse translation = -R^T * t. (The guest does the 3x3 transpose with
 * VU0 pextlw/pcpyld lane shuffles and the -R^T*t with a vmul/vmadd/vsub;
 * reauthored here as plain column-major arithmetic. Validated by
 * inverse(world)*world == identity in the unit test, not bit-matched to
 * the VU0 shuffle routine, which is below the math seam.) */
dmat4 doc_camera_rigid_inverse(dmat4 world) {
    /* 3x3 rotation entries (column-major: M(r,c) = m[c*4+r]). */
    float r00 = world.m[0], r10 = world.m[1], r20 = world.m[2];
    float r01 = world.m[4], r11 = world.m[5], r21 = world.m[6];
    float r02 = world.m[8], r12 = world.m[9], r22 = world.m[10];
    float tx = world.m[12], ty = world.m[13], tz = world.m[14];

    dmat4 inv = dmat4_identity();
    /* transpose of the 3x3 */
    inv.m[0] = r00; inv.m[1] = r01; inv.m[2] = r02;
    inv.m[4] = r10; inv.m[5] = r11; inv.m[6] = r12;
    inv.m[8] = r20; inv.m[9] = r21; inv.m[10] = r22;
    /* -R^T * t */
    inv.m[12] = -(r00 * tx + r10 * ty + r20 * tz);
    inv.m[13] = -(r01 * tx + r11 * ty + r21 * tz);
    inv.m[14] = -(r02 * tx + r12 * ty + r22 * tz);
    return inv;
}

dmat4 doc_camera_view_matrix(const DocCamera *c) {
    dmat4 world = doc_camera_view_rotation(c);   /* camera-to-world rotation */
    world.m[12] = c->eye.x;                      /* + eye translation       */
    world.m[13] = c->eye.y;
    world.m[14] = c->eye.z;
    return doc_camera_rigid_inverse(world);      /* -> world-to-camera      */
}

/* FUN_00263968: PS2 "viewscreen" perspective matrix. Pure float, exactly
 * as the guest (near clamped to >= 1). Writes the guest float indices into
 * the dmat4 (see layout note). */
dmat4 doc_camera_projection(float screen_w, float screen_h,
                            float proj_dist, float znear, float zfar) {
    dmat4 r = { { 0 } };
    float n = znear < 1.0f ? 1.0f : znear;   /* if (param_4 < 1.0) param_4 = 1.0 */
    float f = zfar;
    /* guest: [0]=2d/w [5]=2d/h [10]=(f+n)/(f-n) [11(b)]=1 [14(e)]=-2fn/(f-n) */
    r.m[0]  = (proj_dist + proj_dist) / screen_w;
    r.m[5]  = (proj_dist + proj_dist) / screen_h;
    r.m[10] = (f + n) / (f - n);
    r.m[11] = 1.0f;                           /* guest pfVar1[0xb] */
    r.m[14] = (f * n * -2.0f) / (f - n);      /* guest pfVar1[0xe] */
    r.m[15] = 0.0f;                           /* guest pfVar1[0xf] */
    return r;
}

dmat4 doc_camera_projection_matrix(const DocCamera *c) {
    return doc_camera_projection(c->screen_w, c->screen_h,
                                 c->proj_dist, c->znear, c->zfar);
}

/* FUN_002d9cd0: extract Euler angles (radians) from a rotation matrix.
 * Reads guest indices [0],[4],[5],[6],[8],[9],[10] (== dmat4.m[] here).
 * The guest then scales each result by 651.8988 (= 4096/2pi) to its
 * fixed-point units; we return radians. */
dvec3 doc_camera_matrix_to_angles(dmat4 rot) {
    const float *p = rot.m;
    float ax, ay, az;

    ay = asinf(p[8]);                 /* fVar3 = asin(m[8])  -> angle Y */
    float cay = cosf(ay);             /* fVar4 */
    ax = -atan2f(p[9], p[10]);        /* fVar2 = -atan2(m[9], m[10]) -> X */

    if (cay == 0.0f) {                /* gimbal lock */
        ax = -atan2f(p[6], p[5]);     /* -atan2(m[6], m[5]) */
        az = 0.0f;
    } else {
        float t = asinf(p[4] / cay);  /* fVar4 = asin(m[4]/cos(Y)) */
        az = -t;                      /* fVar5 = -fVar4 */
        if (p[0] < 0.0f)              /* if (fVar9 < 0) */
            az = 3.14159265358979f - az;
    }
    return dvec3_make(ax, ay, az);
}
