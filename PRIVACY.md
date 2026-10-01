# FaceLogin privacy statement

This statement describes the implementation in this repository, including the
development branch's optional auxiliary learning feature. A release may not include
every development feature. Last reviewed: 2026-10-01.

## Local processing and stored information

FaceLogin processes camera frames locally to detect faces, estimate pose, compare
face embeddings and, when enabled, perform liveness checks. No remote recognition
service is used. Live preview frames are not intentionally retained as a continuous recording.

- Enrollment stores account identity information (such as username, UPN and SID),
  face labels and numerical face embeddings in `data/users.dat`.
- The supplied Windows password is encrypted using machine-scope Windows DPAPI.
  DPAPI protects the password field, **not the entire database**. Account metadata
  and face embeddings are not encrypted by that mechanism.
- When the user enables failed-recognition capture, the service stores JPEG camera
  images and event information in `data/unknown`. Images may contain surrounding
  people and background details. This option is disabled by default.
- When a user explicitly adds failed captures to auxiliary learning, the development
  version stores sample images, embeddings, associations and learning prototypes
  under `data/adaptive`. These files are not DPAPI-encrypted.
- Configuration is stored locally. Operational logs and user-saved diagnostic reports
  may include system, device, path and error information; review them before sharing.
- WebView2 maintains browser/runtime data in its configured local data directory.

Installation-directory access controls and Windows account security are important.
Machine-scope DPAPI is not protection against an administrator or a compromised system.
FaceLogin does not sell these records or use them to train a remote model.

## Network and third-party services

FaceLogin's own authentication, enrollment and learning code does not automatically
upload passwords, camera frames, face embeddings, failed captures or logs to the
maintainers or a cloud recognition service. In-app image addresses such as
`https://facelogin-captures/` and `https://facelogin-learning/` map to local folders
through WebView2; they are not FaceLogin-operated internet endpoints.

Opening project links in a browser, downloading releases or dependencies, and
submitting an issue are user-initiated interactions with external services.
Windows and Microsoft's WebView2 runtime may independently contact Microsoft for
runtime updates or other runtime functions under their own policies. This statement
does not promise that the operating system or WebView2 makes no network requests.
See [Microsoft's privacy statement](https://privacy.microsoft.com/privacystatement)
and [WebView2 data and privacy](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/data-privacy).

An application to SignPath separately shares the applicant's name, email and public
project information with SignPath. Any future signing integration transfers build
artifacts and provenance, not installed users' credentials or biometric records.
See [SignPath's privacy policy](https://signpath.io/privacy-policy).

## Removal and support

Use the Console to remove enrolled faces, failed captures or auxiliary learning
samples. Disabling capture stops new failed-image collection; it does not delete
existing images. Removing a record does not remove copies or backups you made.
The uninstaller offers removal of FaceLogin data; choose the appropriate option and
back up information you wish to keep. File deletion is not a secure disk-erasure guarantee.

Do not upload passwords, `users.dat`, learning databases or identifiable camera
photos to public issues. Share only necessary, reviewed and redacted diagnostics.
Contact the project through its [GitHub repository](https://github.com/EthanZer0/FaceLogin).
