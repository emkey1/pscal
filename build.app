• To run the full iOS/iPadOS terminal experience end‑to‑end:

  1. Build the PSCAL static libraries for iOS (from repo root /Users/mke/PBuild):

     cmake --preset ios-simulator
     cmake --build --preset ios-simulator --target pscal_exsh_static
     For hardware builds, repeat with ios-device:

     cmake --preset ios-device
     cmake --build --preset ios-device --target pscal_exsh_static
     (This produces libpscal_exsh_static.a and friends under build/ios-*/ plus generated headers.)

     (This produces libpscal_exsh_static.a and friends under build/ios-*/ plus generated headers.)
  2. Open the SwiftUI host app in Xcode and run it:

     open ios/PscalApp.xcodeproj
      - Select an iPad simulator (e.g., “iPad Pro (11-inch) (4th generation)”).
      - Build & run. The app uses the static lib you built in step 1, spawns exsh_main via the PTY bridge, and shows the VT100-rendered terminal. All typing happens inline at the prompt—
        there’s no auxiliary text field.
  3. (Optional) Deploy to a device:
      - Ensure the ios-device preset build from step 1 has run.
      - In Xcode, switch the scheme to a connected iPad, set your signing team/bundle ID, and build/run. The app links against the device-side static libraries in build/ios-device.

  That’s it—no additional CLI steps are required once the libraries are built.
