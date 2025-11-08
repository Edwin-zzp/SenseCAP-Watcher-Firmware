# Camera Uploader Example (WIP)

This example project prepares the scaffolding for capturing preview frames from
the SenseCAP Watcher camera module and uploading them to an image hosting
service. It currently focuses on the infrastructure pieces (Wi-Fi connection,
FreeRTOS tasks, and data queues) so that image acquisition and HTTP uploading
logic can be implemented iteratively.

## Next steps

- Implement `camera_acquire_latest_frame()` in
  [`main/camera_uploader.c`](main/camera_uploader.c) so it retrieves base64 JPEG
  previews from `tf_module_ai_camera`.
- Extend `uploader_task()` to authenticate with the target hosting API and post
  the captured images.
- Add configuration options (e.g., Wi-Fi credentials, API tokens, upload URL)
  via Kconfig or other provisioning mechanisms.
- Document the final workflow and required credentials once the upload logic is
  in place.
