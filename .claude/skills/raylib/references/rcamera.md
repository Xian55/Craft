# raylib 5.5 — module: rcamera

Exact signatures from the pinned raylib.h. `RLAPI` prefix stripped.

## Camera System Functions

    void UpdateCamera(Camera *camera, int mode);  // Update camera position for selected mode
    void UpdateCameraPro(Camera *camera, Vector3 movement, Vector3 rotation, float zoom);  // Update camera movement/rotation
