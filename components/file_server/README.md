# SD-card web file manager

Open the HTTP address shown on the launcher after Wi-Fi connects. The dark web
interface supports folder navigation, breadcrumbs, back/up, new folders,
multi-file uploads (including drag-and-drop), search, selection, rename,
cut/paste moves, downloads, and permanent deletion of files and empty folders.
Upload destinations are the currently open folder, not just `/` or `/apps`.
Apps intended for the launcher must still be placed in `/apps`.

The HTML/CSS/JavaScript lives in `web/index.html`, embedded into the firmware.
`file_manager.c` handles storage and HTTP; `file_server.c` handles Wi-Fi/server
lifecycle. Successful mutations refresh the device's app list through the
existing dialog/transfer notification queue.

API paths are relative to the SD root, beginning with `/`, and URL-encoded:

- `GET /api/list?path=/music`: streamed JSON folder listing.
- `GET /api/download?path=/music/song.mp3`: streamed file content.
- `POST /api/upload?path=/music/song.mp3`: raw file bytes as the request body.
- `POST /api/mkdir?path=/music/Albums`: create one folder in an existing parent.
- `POST /api/move?path=/music/song.mp3&to=/music/Albums/song.mp3`: move or rename.
- `POST /api/delete?path=/music/song.mp3`: delete a file or empty folder.

Existing destinations produce HTTP 409; uploads/moves never silently overwrite.
Uploads use a hidden same-folder staging file, renamed only after the complete
body is written and closed. Failed transfers remove the staging file. Power loss
can leave a hidden staging file, reused by the next upload into that folder.
The storage guard serializes HTTP operations against OTA flashing.
The parser and 2 KB transfer buffer share a checked, heap-allocated workspace
owned by that guard and freed after each request. The HTTP task has a 12 KB stack;
completed file operations log its minimum remaining stack in bytes. Compiler
stack-usage reports are generated beside component object files for auditing.
SD-root
deletion, traversal, hidden paths, FAT short-name aliases to hidden files, and
invalid/reserved FAT characters are rejected. Paths are limited to 239 UTF-8
bytes. Hidden Wi-Fi backup files are not exposed through this API.

Folders must be empty before deletion. Folder upload/archive download and
copying are not supported. Browser downloads are buffered in browser memory
(the ESP32 streams with a fixed buffer). This is a trusted-local-network tool,
not an authenticated or Internet-facing server.

Run UI interaction regression tests from the project root:

```text
node tests/file_manager_ui.test.cjs
```

Build with the project's activated ESP-IDF environment using `idf.py build`.
Hardware verification after flashing should include nested uploads, an
interrupted upload, duplicate-name conflicts, rename/move, file download/hash
comparison, empty/non-empty-folder deletion, and missing-SD-card errors.
