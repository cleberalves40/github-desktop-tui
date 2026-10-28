class GithubDesktopTui < Formula
  desc "GitHub-oriented terminal UI client"
  homepage "https://github.com/cleberalves40/github-desktop-tui"
  url "https://github.com/cleberalves40/github-desktop-tui/archive/refs/tags/v0.7.0.tar.gz"
  sha256 "e613abba56633c0c94988cdeab82faeac64917836e4bf09dd2bd534a5fa420bb"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "ftxui"
  depends_on "gh"
  depends_on "git"

  def install
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           *std_cmake_args, "-DBUILD_TESTING=OFF"
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_equal "github-desktop-tui #{version}",
                 shell_output("#{bin}/github-desktop-tui --version").strip
  end
end
