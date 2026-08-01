class Chopin < Formula
  desc "GNU cp reimplementation: byte-faithful to coreutils 9.11, parallel copy engine"
  homepage "https://github.com/tenseleyFlow/chopin"
  url "https://github.com/tenseleyFlow/chopin/releases/download/v0.1.0/chopin-0.1.0.tar.gz"
  sha256 "PENDING_PUBLISHED_ASSET"
  license "GPL-3.0-or-later"
  head "https://github.com/tenseleyFlow/chopin.git", branch: "trunk"

  def install
    system "./configure"
    system "make"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    assert_match "chopin #{version}", shell_output("#{bin}/chopin --version")

    # A real copy, verified by content and by mode.
    (testpath/"src").mkpath
    (testpath/"src/a").write "alpha\n"
    (testpath/"src/b").write "beta\n"
    chmod 0640, testpath/"src/b"
    system bin/"chopin", "-a", testpath/"src", testpath/"dst"
    assert_equal "alpha\n", (testpath/"dst/a").read
    assert_equal "beta\n", (testpath/"dst/b").read
    assert_equal 0640, (testpath/"dst/b").stat.mode & 0777

    # The destination re-read oracle must agree on the fast paths.
    system({ "CHOPIN_DEBUG_VERIFY" => "1" }, bin/"chopin",
           testpath/"src/a", testpath/"verified")
    assert_equal "alpha\n", (testpath/"verified").read

    # cpn is the same binary; the name only changes the diagnostic prefix.
    output = shell_output("#{bin}/cpn /nonexistent #{testpath}/x 2>&1", 1)
    assert_match "cpn:", output
  end
end
