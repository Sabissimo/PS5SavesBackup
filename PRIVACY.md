# PS5SavesBackup privacy policy

PS5SavesBackup is open-source homebrew software that runs on your own PlayStation 5 and backs up your game saves to storage you choose. It is not a service: there is no server operated by the project, no account, and no telemetry.

## What the software does with your data

- **USB backups** are written to the USB partition you select on the settings page. The data never leaves your console and your drive.
- **Google Drive backups** (optional, when enabled) are uploaded directly from your console to your own Google Drive account using the Google Drive API. The software requests the `drive.file` permission only, which limits it to files and folders it creates itself. It cannot see, change or delete your other Drive files.
- The Google sign-in happens on Google's own pages (the "device code" flow). The software receives an access token and a refresh token from Google and stores them only on your console, in `/data/homebrew/PS5SavesBackup/`. Your Google password is never seen by the software.
- Nothing is sent to the project authors or to any third party. The software makes network connections only to Google's API servers, and only when Google Drive backups are enabled.

## Data stored on the console

- Your settings (`backup.conf`), the backup change cache and, if Google Drive is enabled, the Google tokens, all under `/data/homebrew/PS5SavesBackup/`.
- A marker file `PS5SavesBackup.id` on the selected USB partition.

## Revoking access

Remove the Google tokens by disabling Google Drive on the settings page or deleting the token file, and revoke the app's permission at https://myaccount.google.com/permissions.

## Contact

Open an issue in the project's GitHub repository.
