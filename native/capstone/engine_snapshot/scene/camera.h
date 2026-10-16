/* =====================================================================
 * camera.h  --  Reauthored CAMERA / VIEW subsystem (purist port, P4).
 *
 * The camera is "the view the renderer needs + the state the scripts
 * control". It owns: the camera STATE (eye position, Euler orientation,
 * field-of-view, clip planes), the VIEW matrix build (Euler -> rotation
 * -> rigid inverse = world->camera), and the PROJECTION matrix build
 * (the PS2 "viewscreen" perspective). These two matrices are what the
 * EE hands the VU1 transform each frame.
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   FUN_00259928  Euler X-axis rotation matrix   -> doc_camera_rot_x
 *   FUN_00259990  Euler Y-axis rotation matrix   -> doc_camera_rot_y
 *   FUN_002599f8  Euler Z-axis rotation matrix   -> doc_camera_rot_z
 *   FUN_00259c50  compose R = R[o2]*R[o1]*R[o0]  -> doc_camera_view_rotation
 *   FUN_002610e0  rigid inverse (world->view)    -> doc_camera_rigid_inverse
 *   FUN_00263968  PS2 perspective matrix         -> doc_camera_projection
 *   FUN_002d9cd0  rotation matrix -> Euler (deg)  -> doc_camera_matrix_to_angles
 * Helpers traced: FUN_002613e0 (3x3 identity), FUN_002613b8 /
 * FUN_00444d30 (4x4 identity), FUN_0026125c (VU0 4x4 multiply = dmat4_mul),
 * FUN_003ede00 (4x4 copy). Trig: FUN_0045b530=sinf, FUN_0045b428=cosf,
 * FUN_0045b7a8=asinf, FUN_0045b8a8=atan2f (confirmed by the "asinf"
 * literal and the <pi/4 argument-reduction thresholds).
 *
 * Camera STATE / bridge (analysis/decomp + native/script/engine_bridge_api.txt):
 *   Camera manager block  = DAT_0047d8c0 (alloc 0x340 B, FUN_003ee628).
 *     [0]            active camera index (FUN_003ee608 / FUN_003ee5a0).
 *     +0x10 + i*0xe0 camera entry i (FUN_003ee608 returns the active one).
 *   Per-frame "current camera" cache globals (written by FUN_002dacf8,
 *   read by the projection path FUN_0026afe8 and flush FUN_003d3cd0):
 *     DAT_004fcb10..b4c  4x4 camera/view matrix (16 floats, row-major).
 *     DAT_004fcacc       field-of-view scalar  (fov; *11.377778 ~= 2048/180
 *                        deg->half-screen-units -> projection distance).
 *     DAT_004fcac8       aspect / y-scale factor.
 *     DAT_004fcab8/abc   screen width / height feeding FUN_00263968.
 *   DoC angles are fixed-point: 4096 units == 2*pi (the 651.8988 = 4096/2pi
 *   scale in FUN_002d9cd0; wrap via FUN_002d9208 mod 4096).
 *
 * The KerberosCamera.* script natives (engine_bridge_api.txt lines 77-90)
 * are backed at the STATE level by this module (see the mapping comments
 * on each setter/getter). The JVM native-method REGISTRY that binds the
 * literal "fake/KerberosCamera.setfov" string to the engine entry is a
 * documented engine GAP (engine-map.md section 3) and is NOT reverse-
 * engineered here; we model what each method reads/writes, not the dispatch
 * address. Everything below the "upload matrix to VU1 via DMA/GIF" seam
 * (FUN_002dacf8 VU0 packing, FUN_0040d340/0040d670 draw-env) is left to the
 * renderer subsystem.
 * ===================================================================== */
#ifndef DOC_CAMERA_H
#define DOC_CAMERA_H

#include "dmath.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guest fixed-point angle unit: a full turn is 4096 units.
 * (651.8988f == 4096 / (2*pi), the radians->units scale in FUN_002d9cd0.) */
#define DOC_ANGLE_UNITS_PER_TURN 4096.0f

static inline float doc_angle_units_to_radians(float units) {
    return units * (6.28318530717958647692f / DOC_ANGLE_UNITS_PER_TURN);
}
static inline float doc_angle_radians_to_units(float radians) {
    return radians * (DOC_ANGLE_UNITS_PER_TURN / 6.28318530717958647692f);
}

/* Axis identifiers for the Euler rotation builders (match the index the
 * camera's rotation-order table passes to FUN_00259a60: 0=X, 1=Y, 2=Z). */
typedef enum {
    DOC_AXIS_X = 0,
    DOC_AXIS_Y = 1,
    DOC_AXIS_Z = 2
} DocAxis;

/* The camera / view state.
 *
 * Orientation is stored as three Euler angles in RADIANS plus the order in
 * which the per-axis rotations are composed (the game reads a per-camera
 * order table; the common DoC order is Z,Y,X -> order = {X,Y,Z} applied
 * so that R = Rz*Ry*Rx, see doc_camera_view_rotation). The renderer path
 * also keeps an eye+target+up view (KerberosCamera.set / .getCurrentCamera-
 * Vectors); target/up are derived from eye + orientation on demand. */
typedef struct DocCamera {
    /* ---- STATE the scripts control ---- */
    dvec3   eye;              /* world position (KerberosCamera getpos/move) */
    dvec3   angles;          /* Euler X,Y,Z in radians                      */
    DocAxis order[3];        /* rotation apply order (R = R[o2]*R[o1]*R[o0])*/
    float   fov;             /* field of view, radians (KerberosCamera setfov)*/

    /* ---- projection parameters (the "viewscreen" build) ---- */
    float   screen_w;        /* <- DAT_004fcab8 (projection width)          */
    float   screen_h;        /* <- DAT_004fcabc (projection height)         */
    float   proj_dist;       /* near-plane projection reference distance    */
    float   znear;           /* near clip (clamped to >= 1, FUN_00263968)   */
    float   zfar;            /* far clip                                    */
} DocCamera;

/* Initialise a camera to engine defaults (identity view, Z,Y,X order). */
void doc_camera_init(DocCamera *c);

/* ---- STATE setters/getters backing the KerberosCamera bridge ---------- */

/* KerberosCamera getpos:()Lfake/base/KerberosVector;  -> read eye.         */
dvec3 doc_camera_get_position(const DocCamera *c);

/* KerberosCamera.set:(V eye, V target, V up)  -> set eye and derive the
 * Euler orientation that looks from eye toward target (up selects roll).
 * Uses the FUN_002d9cd0 matrix->Euler extraction on the look basis. */
void doc_camera_set(DocCamera *c, dvec3 eye, dvec3 target, dvec3 up);

/* KerberosCamera.move:(FFF)  -> translate eye by (dx,dy,dz).               */
void doc_camera_move(DocCamera *c, float dx, float dy, float dz);

/* KerberosCamera.setfov:(F)  -> store field of view (radians).            */
void doc_camera_set_fov(DocCamera *c, float fov_radians);

/* Set the Euler orientation directly (radians).                           */
void doc_camera_set_angles(DocCamera *c, dvec3 angles_radians);

/* KerberosCamera.getCurrentCameraVectors -> eye, forward target, and up,
 * reconstructed from eye + orientation. out[3] = {eye, target, up}.       */
void doc_camera_get_vectors(const DocCamera *c, dvec3 out_eye_target_up[3]);

/* ---- matrix builds (what the renderer/VU1 consumes) ------------------- */

/* FUN_00259928/990/f8: a single-axis rotation matrix for `angle` radians.
 * Returned as a column-major dmat4 (the transpose of the game's row-major
 * build; same transform for the column-vector v' = M*v convention). */
dmat4 doc_camera_rot_x(float angle);
dmat4 doc_camera_rot_y(float angle);
dmat4 doc_camera_rot_z(float angle);
dmat4 doc_camera_axis_rotation(DocAxis axis, float angle);

/* FUN_00259c50: compose the three Euler rotations in the camera's order,
 * R = R[order[2]] * R[order[1]] * R[order[0]].  (Column-major.) */
dmat4 doc_camera_view_rotation(const DocCamera *c);

/* FUN_002610e0: rigid inverse of a camera-to-world transform (orthonormal
 * rotation R + translation t) -> world-to-camera: R^T and -R^T*t. */
dmat4 doc_camera_rigid_inverse(dmat4 world);

/* Full VIEW matrix (world -> camera): rotation from the Euler angles with
 * the eye translation, then rigid-inverted. Column-major dmat4. */
dmat4 doc_camera_view_matrix(const DocCamera *c);

/* FUN_00263968: the PS2 "viewscreen" perspective matrix.
 * Row-major (guest) layout, element (row r, col c) at out[r*4+c]:
 *   [ 2d/w   0      0            0 ]
 *   [ 0      2d/h   0            0 ]
 *   [ 0      0      (f+n)/(f-n)  1 ]
 *   [ 0      0     -2fn/(f-n)    0 ]
 * where d = proj_dist, w/h = screen extents, n (clamped >= 1) = near,
 * f = far. Returned column-major (transposed). */
dmat4 doc_camera_projection(float screen_w, float screen_h,
                            float proj_dist, float znear, float zfar);

/* Convenience: projection from the camera's stored parameters. */
dmat4 doc_camera_projection_matrix(const DocCamera *c);

/* FUN_002d9cd0: extract Euler angles (RADIANS) from a 3x3 rotation held in
 * a column-major dmat4. Returns {x,y,z}. The game scales these by 4096/2pi
 * to its fixed-point angle units; use doc_angle_radians_to_units for that. */
dvec3 doc_camera_matrix_to_angles(dmat4 rot);

#ifdef __cplusplus
}
#endif
#endif /* DOC_CAMERA_H */
