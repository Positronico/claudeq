# Homebrew formula for the Claudeq bridge + launcher.
#
# Lives in the tap repo github.com/Positronico/homebrew-tap as Formula/claudeq.rb.
# Users install with:
#   brew install Positronico/tap/claudeq
#
# To cut a new version: tag the main repo (vX.Y.Z), then update `url` + `sha256` here
# (sha256 = `curl -L <url> | shasum -a 256`). See PUBLISHING.md.
class Claudeq < Formula
  desc "Physical touchscreen control surface for Claude Code (ESP32-S3 deck)"
  homepage "https://github.com/Positronico/claudeq"
  url "https://github.com/Positronico/claudeq/archive/refs/tags/v2.1.5.tar.gz"
  sha256 "8359d25f86feb35d2b64f2edeb3439a888bd58abb6d58e65420d17ceab039148"
  license "MIT"

  depends_on "node"   # runtime for the bridge
  depends_on "tmux"   # macro/voice injection into Claude sessions
  depends_on "ffmpeg" # alert-sound conversion

  def install
    libexec.install "bridge"
    # Only the pre-built firmware is needed at runtime (for `claudeq flash`), not the IDF sources.
    (libexec/"firmware").install "firmware/dist"

    # The two tiny runtime deps (ws, bonjour-service) are vendored in the tarball, so install is
    # offline (Homebrew sandboxes network during install). Restore from the lock only if absent.
    unless (libexec/"bridge/node_modules").exist?
      cd(libexec/"bridge") { system "npm", "ci", "--omit=dev", "--no-audit", "--no-fund" }
    end

    # Wrapper execs the REAL script path so claudeq's BASH_SOURCE-based lookup of its siblings
    # (bridge.mjs, ccdeck-settings.json, ../firmware/dist/flash.sh) resolves inside libexec.
    # Note: we deliberately do NOT expose the in-repo `cc` shim — it would shadow the C compiler.
    (bin/"claudeq").write <<~SH
      #!/bin/bash
      exec "#{libexec}/bridge/claudeq" "$@"
    SH
  end

  def caveats
    <<~EOS
      Flash the device (no toolchain needed):
        • Browser:  https://positronico.github.io/claudeq/  (Chrome/Edge → Install)
        • CLI:      claudeq flash      (uses esptool/espflash if present)

      Use it, in any project:
        cd your-project && claudeq

      Optional local voice transcription:
        brew install whisper-cpp   # then see the README "Voice" setup
    EOS
  end

  test do
    # The merged firmware must be bundled for `claudeq flash` to work.
    assert_predicate libexec/"firmware/dist/claudeq-firmware.bin", :exist?

    # Hermetic check of the wrapper -> flash.sh delegation: a bogus CLAUDEQ_FIRMWARE makes flash.sh
    # exit immediately, before any serial-port autodetect or esptool/espflash spawn (which could hang
    # if a flasher happens to be on PATH). Proves claudeq execs the bundled flash.sh and reads its env.
    output = shell_output("CLAUDEQ_FIRMWARE=/nonexistent.bin #{bin}/claudeq flash 2>&1", 1)
    assert_match(/firmware image not found/i, output)
  end
end
