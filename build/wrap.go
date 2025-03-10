//go:build none
// +build none

package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"text/template"
)

// nobuild can be used to prevent the wrappers from triggering a build after
// each step. This should only be used in production mode when there's a final
// build check outside of the wrapping.
var nobuild = flag.Bool("nobuild", false, "Prevents the wrappers from building")
var genLock = flag.Bool("update", false, "Pulls new commits, if unset the libs commits will be taken from lock.json.")

func main() {
	flag.Parse()
	var lock *lockJson
	if !*genLock {
		lock = &lockJson{}
		f, err := os.Open("lock.json")
		if err != nil {
			panic(err)
		}
		err = json.NewDecoder(f).Decode(lock)
		f.Close()
		if err != nil {
			panic(err)
		}
	}

	// TarGeT stores the target to generate, the idea is a target is block of oses
	// compatible with each others (Linux and Android, OSX and IOS)
	var tgt string
	switch runtime.GOOS {
	case "linux", "android":
		tgt = "linux"
	case "darwin":
		tgt = "darwin"
	default:
		panic(fmt.Errorf("Sorry but your os : %s is not yet supported.", runtime.GOOS))
	}

	// Clean up any previously generated files
	if _, err := os.Stat("libtor"); !os.IsNotExist(err) && *genLock {
		os.RemoveAll("libtor")
	}
	// Do the same in the target directory
	if _, err := os.Stat(tgt); !os.IsNotExist(err) {
		os.RemoveAll(tgt)
	}
	// Copy in the library preamble with the architecture definitions
	if err := os.MkdirAll("libtor", 0755); err != nil {
		panic(err)
	}
	blob, _ := ioutil.ReadFile(filepath.Join("build", "libtor_preamble.go.in"))
	ioutil.WriteFile(filepath.Join("libtor", "libtor_preamble.go"), blob, 0644)

	// Create target directory
	if err := os.MkdirAll(tgt, 0755); err != nil {
		panic(err)
	}

	// Wrap each of the component libraries into megator
	zlibVer, zlibHash, err := wrapZlib(tgt, lock)
	if err != nil {
		panic(err)
	}
	libeventVer, libeventHash, err := wrapLibevent(tgt, lock)
	if err != nil {
		panic(err)
	}
	opensslVer, opensslHash, err := wrapOpenSSL(tgt, lock)
	if err != nil {
		panic(err)
	}
	torVer, torHash, err := wrapTor(tgt, lock)
	if err != nil {
		panic(err)
	}

	// Copy and fill out the libtor entrypoint wrappers and the readme template.
	blob, _ = ioutil.ReadFile(filepath.Join("build", "libtor_external.go.in"))
	ioutil.WriteFile(filepath.Join("libtor.go"), blob, 0644)
	blob, _ = ioutil.ReadFile(filepath.Join("build", "libtor_internal.go.in"))
	ioutil.WriteFile(filepath.Join("libtor", "libtor.go"), blob, 0644)

	if !*nobuild {
		builder := exec.Command("go", "build", ".")
		builder.Stdout = os.Stdout
		builder.Stderr = os.Stderr

		if err := builder.Run(); err != nil {
			panic(err)
		}
	}

	// Update
	if *genLock {
		tmpl := template.Must(template.ParseFiles(filepath.Join("build", "README.md")))
		buf := new(bytes.Buffer)
		tmpl.Execute(buf, map[string]string{
			"zlibVer":      zlibVer,
			"zlibHash":     zlibHash,
			"libeventVer":  libeventVer,
			"libeventHash": libeventHash,
			"opensslVer":   opensslVer,
			"opensslHash":  opensslHash,
			"torVer":       torVer,
			"torHash":      torHash,
		})
		ioutil.WriteFile("README.md", buf.Bytes(), 0644)
		buff, err := json.MarshalIndent(lockJson{
			Zlib:     zlibHash,
			Libevent: libeventHash,
			Openssl:  opensslHash,
			Tor:      torHash,
		}, "", "  ")
		if err != nil {
			panic(err)
		}
		buff = append(buff, '\n')
		ioutil.WriteFile("lock.json", buff, 0644)
	}
}

// targetFilters maps a build target to the builds tags to apply to it
var targetFilters = map[string]string{
	"linux":  "linux android",
	"darwin": "darwin,amd64 darwin,arm64 ios,amd64 ios,arm64",
}

// lockJson stores the commits for later reuse.
type lockJson struct {
	Zlib     string `json:"zlib"`
	Libevent string `json:"libevent"`
	Openssl  string `json:"openssl"`
	Tor      string `json:"tor"`
}

// wrapZlib clones the zlib library into the local repository and wraps it into
// a Go package.
//
// Zlib is a small and simple C library which can be wrapped by inserting an empty
// Go file among the C sources, causing the Go compiler to pick up all the loose
// sources and build them together into a static library.
func wrapZlib(tgt string, lock *lockJson) (string, string, error) {
	// TarGeT Full
	tgtf := filepath.Join(tgt, "zlib")

	cloner := exec.Command("git", "clone", "https://github.com/madler/zlib")
	cloner.Stdout = os.Stdout
	cloner.Stderr = os.Stderr
	cloner.Dir = tgt

	if err := cloner.Run(); err != nil {
		return "", "", err
	}

	// If we have a commit lock, checkout these commits.
	if lock != nil {
		checkouter := exec.Command("git", "checkout", lock.Zlib)
		checkouter.Dir = tgtf

		if err := checkouter.Run(); err != nil {
			return "", "", err
		}
	}

	// Save the latest upstream commit hash for later reference
	parser := exec.Command("git", "rev-parse", "HEAD")
	parser.Dir = tgtf

	commit, err := parser.CombinedOutput()
	if err != nil {
		fmt.Println(string(commit))
		return "", "", err
	}
	commit = bytes.TrimSpace(commit)

	// Retrieve the version of the current commit
	conf, _ := ioutil.ReadFile(filepath.Join(tgtf, "zlib.h"))
	strver := regexp.MustCompile("define ZLIB_VERSION \"(.+)\"").FindSubmatch(conf)[1]

	// Wipe everything from the library that's non-essential
	files, err := ioutil.ReadDir(tgtf)
	if err != nil {
		return "", "", err
	}
	for _, file := range files {
		if file.IsDir() {
			os.RemoveAll(filepath.Join(tgtf, file.Name()))
			continue
		}
		if ext := filepath.Ext(file.Name()); ext != ".h" && ext != ".c" {
			os.Remove(filepath.Join(tgtf, file.Name()))
		}
	}

	// TarGeTFILTer
	tgtFilt := targetFilters[tgt]

	// Generate Go wrappers for each C source individually
	tmpl, err := template.New("").Parse(zlibTemplate)
	if err != nil {
		return "", "", err
	}
	for _, file := range files {
		if file.IsDir() {
			continue
		}
		if ext := filepath.Ext(file.Name()); ext == ".c" {
			name := strings.TrimSuffix(file.Name(), ext)
			buff := new(bytes.Buffer)
			if err := tmpl.Execute(buff, map[string]string{
				"TargetFilter": tgtFilt,
				"File":         name,
			}); err != nil {
				return "", "", err
			}
			ioutil.WriteFile(filepath.Join("libtor", tgt+"_zlib_"+name+".go"), buff.Bytes(), 0644)
		}
	}

	tmpl, err = template.New("").Parse(zlibPreamble)
	if err != nil {
		return "", "", err
	}
	buff := new(bytes.Buffer)
	if err := tmpl.Execute(buff, map[string]string{
		"TargetFilter": tgtFilt,
		"Target":       tgt,
	}); err != nil {
		return "", "", err
	}
	ioutil.WriteFile(filepath.Join("libtor", tgt+"_zlib_preamble.go"), buff.Bytes(), 0644)
	return string(strver), string(commit), nil
}

// zlibPreamble is the CGO preamble injected to configure the C compiler.
var zlibPreamble = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor


/*
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/zlib
#cgo CFLAGS: -DHAVE_UNISTD_H -DHAVE_STDARG_H
*/
import "C"
`

// zlibTemplate is the source file template used in zlib Go wrappers.
var zlibTemplate = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor

/*
#include <../zlib/{{.File}}.c>
*/
import "C"
`

// wrapLibevent clones the libevent library into the local repository and wraps
// it into a Go package.
//
// Libevent is a fairly straightforward C library, however it heavily relies on
// makefiles to mix-and-match the correct sources for the correct platforms. It
// also relies on autoconf and family to generate platform specific configs.
//
// Since it's not meaningfully feasible to build libevent without the make tools,
// yet that approach cannot create a portable Go library, we're going to hook
// into the original build mechanism and use the emitted events as a driver for
// the Go wrapping.
func wrapLibevent(tgt string, lock *lockJson) (string, string, error) {
	// TarGeT Full
	tgtf := filepath.Join(tgt, "libevent")

	cloner := exec.Command("git", "clone", "https://github.com/libevent/libevent")
	cloner.Stdout = os.Stdout
	cloner.Stderr = os.Stderr
	cloner.Dir = tgt

	if err := cloner.Run(); err != nil {
		return "", "", err
	}

	// If we have a commit lock, checkout these commits.
	if lock != nil {
		checkouter := exec.Command("git", "checkout", lock.Libevent)
		checkouter.Dir = tgtf

		if err := checkouter.Run(); err != nil {
			return "", "", err
		}
	}

	// Save the latest upstream commit hash for later reference
	parser := exec.Command("git", "rev-parse", "HEAD")
	parser.Dir = tgtf

	commit, err := parser.CombinedOutput()
	if err != nil {
		fmt.Println(string(commit))
		return "", "", err
	}
	commit = bytes.TrimSpace(commit)

	// Configure the library for compilation
	autogen := exec.Command("./autogen.sh")
	autogen.Dir = tgtf
	autogen.Stdout = os.Stdout
	autogen.Stderr = os.Stderr

	if err := autogen.Run(); err != nil {
		return "", "", err
	}
	configure := exec.Command("./configure", "--disable-shared", "--enable-static")
	configure.Dir = tgtf
	configure.Stdout = os.Stdout
	configure.Stderr = os.Stderr

	if err := configure.Run(); err != nil {
		return "", "", err
	}
	// Retrieve the version of the current commit
	conf, _ := ioutil.ReadFile(filepath.Join(tgtf, "configure.ac"))
	numver := regexp.MustCompile("AC_DEFINE\\(NUMERIC_VERSION, (0x[0-9]{8}),").FindSubmatch(conf)[1]
	strver := regexp.MustCompile("AC_INIT\\(libevent,(.+)\\)").FindSubmatch(conf)[1]

	// Hook the make system and gather the needed sources
	maker := exec.Command("make", "--dry-run", "libevent.la")
	maker.Dir = tgtf

	out, err := maker.CombinedOutput()
	if err != nil {
		fmt.Println(string(out))
		return "", "", err
	}
	deps := regexp.MustCompile(" ([a-z_]+)\\.lo;").FindAllStringSubmatch(string(out), -1)

	// Wipe everything from the library that's non-essential
	files, err := ioutil.ReadDir(tgtf)
	if err != nil {
		return "", "", err
	}
	for _, file := range files {
		// Remove all folders apart from the headers
		if file.IsDir() {
			if file.Name() == "include" || file.Name() == "compat" {
				continue
			}
			os.RemoveAll(filepath.Join(tgtf, file.Name()))
			continue
		}
		// Remove all files apart from the sources and license
		if file.Name() == "LICENSE" {
			continue
		}
		if ext := filepath.Ext(file.Name()); ext != ".h" && ext != ".c" {
			os.Remove(filepath.Join(tgtf, file.Name()))
		}
	}

	// TarGeTFILTer
	tgtFilt := targetFilters[tgt]

	// Generate Go wrappers for each C source individually
	tmpl, err := template.New("").Parse(libeventTemplate)
	if err != nil {
		return "", "", err
	}
	for _, dep := range deps {
		buff := new(bytes.Buffer)
		if err := tmpl.Execute(buff, map[string]string{
			"TargetFilter": tgtFilt,
			"File":         dep[1],
		}); err != nil {
			return "", "", err
		}
		ioutil.WriteFile(filepath.Join("libtor", tgt+"_libevent_"+dep[1]+".go"), buff.Bytes(), 0644)
	}
	tmpl, err = template.New("").Parse(libeventPreamble)
	if err != nil {
		return "", "", err
	}
	buff := new(bytes.Buffer)
	if err := tmpl.Execute(buff, map[string]string{
		"TargetFilter": tgtFilt,
		"Target":       tgt,
	}); err != nil {
		return "", "", err
	}
	ioutil.WriteFile(filepath.Join("libtor", tgt+"_libevent_preamble.go"), buff.Bytes(), 0644)

	// Inject the configuration headers and ensure everything builds
	os.MkdirAll(filepath.Join("libevent_config", "event2"), 0755)

	for _, arch := range []string{"", ".linux64", ".linux32", ".android64", ".android32", ".macos64", ".ios64"} {
		blob, _ := ioutil.ReadFile(filepath.Join("config", "libevent", fmt.Sprintf("event-config%s.h", arch)))
		tmpl, err := template.New("").Parse(string(blob))
		if err != nil {
			return "", "", err
		}
		buff := new(bytes.Buffer)
		if err := tmpl.Execute(buff, struct{ NumVer, StrVer string }{string(numver), string(strver)}); err != nil {
			return "", "", err
		}
		ioutil.WriteFile(filepath.Join("libevent_config", "event2", fmt.Sprintf("event-config%s.h", arch)), buff.Bytes(), 0644)
	}
	return string(strver), string(commit), nil
}

// libeventPreamble is the CGO preamble injected to configure the C compiler.
var libeventPreamble = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor

/*
#cgo CFLAGS: -I${SRCDIR}/../libevent_config
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/libevent
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/libevent/compat
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/libevent/include
*/
import "C"
`

// libeventTemplate is the source file template used in libevent Go wrappers.
var libeventTemplate = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor

/*
#include <compat/sys/queue.h>
#include <../{{.File}}.c>
*/
import "C"
`

// wrapOpenSSL clones the OpenSSL library into the local repository and wraps
// it into a Go package.
//
// OpenSSL is a fairly complex C library, heavily relying on makefiles to mix-
// and-match the correct sources for the correct platforms and it also relies on
// platform specific assembly sources for more performant builds.
//
// Since it's not meaningfully feasible to build OpenSSL without the make tools,
// yet that approach cannot create a portable Go library, we're going to hook
// into the original build mechanism and use the emitted events as a driver for
// the Go wrapping.
//
// In addition, assembly is disabled altogether to retain Go's portability. This
// is a downside we unfortunately have to live with for now.
func wrapOpenSSL(tgt string, lock *lockJson) (string, string, error) {
	// TarGeT Full
	tgtf := filepath.Join(tgt, "openssl")

	cloner := exec.Command("git", "clone", "https://github.com/openssl/openssl")
	cloner.Stdout = os.Stdout
	cloner.Stderr = os.Stderr
	cloner.Dir = tgt

	if err := cloner.Run(); err != nil {
		return "", "", err
	}

	// OpenSSL is a security concern, switch to the latest stable code
	brancher := exec.Command("git", "branch", "-a")
	brancher.Dir = tgtf

	out, err := brancher.CombinedOutput()
	if err != nil {
		return "", "", err
	}
	stables := regexp.MustCompile("remotes/origin/(OpenSSL_[0-9]_[0-9]_[0-9]-stable)").FindAllSubmatch(out, -1)
	if len(stables) == 0 {
		return "", "", errors.New("no stable branch found")
	}
	var checkout string
	// If we have a commit lock, checkout these commits.
	if lock != nil {
		checkout = lock.Openssl
	} else {
		checkout = string(stables[len(stables)-1][1])
	}
	switcher := exec.Command("git", "checkout", checkout)
	switcher.Dir = tgtf

	if out, err = switcher.CombinedOutput(); err != nil {
		fmt.Println(string(out))
		return "", "", err
	}
	// Save the latest upstream commit hash for later reference
	parser := exec.Command("git", "rev-parse", "HEAD")
	parser.Dir = tgtf

	commit, err := parser.CombinedOutput()
	if err != nil {
		fmt.Println(string(commit))
		return "", "", err
	}
	commit = bytes.TrimSpace(commit)

	//Save the latest
	timer := exec.Command("git", "show", "-s", "--format=%cd")
	timer.Dir = tgtf

	date, err := timer.CombinedOutput()
	if err != nil {
		fmt.Println(string(date))
		return "", "", err
	}
	date = bytes.TrimSpace(date)

	// Extract the version string
	strver := bytes.Replace(stables[len(stables)-1][1], []byte("_"), []byte("."), -1)[len("OpenSSL_"):]

	// Configure the library for compilation
	config := exec.Command("./config", "no-shared", "no-zlib", "no-asm", "no-async", "no-sctp")
	config.Dir = tgtf
	config.Stdout = os.Stdout
	config.Stderr = os.Stderr

	if err := config.Run(); err != nil {
		return "", "", err
	}
	// Hook the make system and gather the needed sources
	maker := exec.Command("make", "--dry-run")
	maker.Dir = tgtf

	if out, err = maker.CombinedOutput(); err != nil {
		fmt.Println(string(out))
		return "", "", err
	}
	deps := regexp.MustCompile("(?m)([a-z0-9_/-]+)\\.c$").FindAllStringSubmatch(string(out), -1)

	// Wipe everything from the library that's non-essential
	files, err := ioutil.ReadDir(tgtf)
	if err != nil {
		return "", "", err
	}
	for _, file := range files {
		// Remove all folders apart from the headers
		if file.IsDir() {
			if file.Name() == "crypto" || file.Name() == "engines" || file.Name() == "include" || file.Name() == "ssl" {
				continue
			}
			os.RemoveAll(filepath.Join(tgtf, file.Name()))
			continue
		}
		// Remove all files apart from the license and sources
		if file.Name() == "LICENSE" {
			continue
		}
		if ext := filepath.Ext(file.Name()); ext != ".h" && ext != ".c" {
			os.Remove(filepath.Join(tgtf, file.Name()))
		}
	}

	// TarGeTFILTer
	tgtFilt := targetFilters[tgt]

	// Generate Go wrappers for each C source individually
	tmpl, err := template.New("").Parse(opensslTemplate)
	if err != nil {
		return "", "", err
	}
	for _, dep := range deps {
		// Skip any files not needed for the library
		if strings.HasPrefix(dep[1], "apps/") {
			continue
		}
		if strings.HasPrefix(dep[1], "fuzz/") {
			continue
		}
		if strings.HasPrefix(dep[1], "test/") {
			continue
		}
		// Anything else is wrapped directly with Go
		gofile := strings.Replace(dep[1], "/", "_", -1) + ".go"
		buff := new(bytes.Buffer)
		if err := tmpl.Execute(buff, map[string]string{
			"TargetFilter": tgtFilt,
			"File":         dep[1],
		}); err != nil {
			return "", "", err
		}
		ioutil.WriteFile(filepath.Join("libtor", tgt+"_openssl_"+gofile), buff.Bytes(), 0644)
	}
	tmpl, err = template.New("").Parse(opensslPreamble)
	if err != nil {
		return "", "", err
	}
	buff := new(bytes.Buffer)
	if err := tmpl.Execute(buff, map[string]string{
		"TargetFilter": tgtFilt,
		"Target":       tgt,
	}); err != nil {
		return "", "", err
	}
	ioutil.WriteFile(filepath.Join("libtor", tgt+"_openssl_preamble.go"), buff.Bytes(), 0644)

	// Inject the configuration headers and ensure everything builds
	os.MkdirAll(filepath.Join("openssl_config", "crypto"), 0755)

	for _, arch := range []string{"", ".linux", ".darwin"} {
		blob, _ := ioutil.ReadFile(filepath.Join("config", "openssl", fmt.Sprintf("dso_conf%s.h", arch)))
		ioutil.WriteFile(filepath.Join("openssl_config", "crypto", fmt.Sprintf("dso_conf%s.h", arch)), blob, 0644)
	}

	for _, arch := range []string{"", ".x64", ".x86"} {
		blob, _ := ioutil.ReadFile(filepath.Join("config", "openssl", fmt.Sprintf("bn_conf%s.h", arch)))
		ioutil.WriteFile(filepath.Join("openssl_config", "crypto", fmt.Sprintf("bn_conf%s.h", arch)), blob, 0644)
	}
	for _, arch := range []string{"", ".x64", ".x86", ".macos64", ".ios64"} {
		blob, _ := ioutil.ReadFile(filepath.Join("config", "openssl", fmt.Sprintf("buildinf%s.h", arch)))
		tmpl, err := template.New("").Parse(string(blob))
		if err != nil {
			return "", "", err
		}
		buff := new(bytes.Buffer)
		if err := tmpl.Execute(buff, struct{ Date string }{string(date)}); err != nil {
			return "", "", err
		}
		ioutil.WriteFile(filepath.Join("openssl_config", fmt.Sprintf("buildinf%s.h", arch)), buff.Bytes(), 0644)
	}
	os.MkdirAll(filepath.Join("openssl_config", "openssl"), 0755)

	for _, arch := range []string{"", ".x64", ".x86", ".macos64", ".ios64"} {
		blob, _ := ioutil.ReadFile(filepath.Join("config", "openssl", fmt.Sprintf("opensslconf%s.h", arch)))
		ioutil.WriteFile(filepath.Join("openssl_config", "openssl", fmt.Sprintf("opensslconf%s.h", arch)), blob, 0644)
	}
	return string(strver), string(commit), nil
}

// opensslPreamble is the CGO preamble injected to configure the C compiler.
var opensslPreamble = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor

/*
#cgo CFLAGS: -I${SRCDIR}/../openssl_config
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/openssl
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/openssl/include
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/openssl/crypto/ec/curve448
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/openssl/crypto/ec/curve448/arch_32
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/openssl/crypto/modes
*/
import "C"
`

// opensslTemplate is the source file template used in OpenSSL Go wrappers.
var opensslTemplate = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor

/*
#define DSO_NONE
#define OPENSSLDIR "/usr/local/ssl"
#define ENGINESDIR "/usr/local/lib/engines"

#include <../{{.File}}.c>
*/
import "C"
`

// wrapTor clones the Tor library into the local repository and wraps it into a
// Go package.
func wrapTor(tgt string, lock *lockJson) (string, string, error) {
	// TarGeT Full
	tgtf := filepath.Join(tgt, "tor")

	cloner := exec.Command("git", "clone", "https://git.torproject.org/tor.git")
	cloner.Stdout = os.Stdout
	cloner.Stderr = os.Stderr
	cloner.Dir = tgt

	if err := cloner.Run(); err != nil {
		return "", "", err
	}

	var checkout string
	// If we have a commit lock, checkout these commits.
	if lock != nil {
		checkout = lock.Tor
	} else {
		checkout = "maint-0.4.7"
	}
	checkouter := exec.Command("git", "checkout", checkout)
	checkouter.Dir = tgtf

	if err := checkouter.Run(); err != nil {
		return "", "", err
	}
	// Save the latest upstream commit hash for later reference
	parser := exec.Command("git", "rev-parse", "HEAD")
	parser.Dir = tgtf

	commit, err := parser.CombinedOutput()
	if err != nil {
		fmt.Println(string(commit))
		return "", "", err
	}
	commit = bytes.TrimSpace(commit)

	// Configure the library for compilation
	autogen := exec.Command("./autogen.sh")
	autogen.Dir = tgtf
	autogen.Stdout = os.Stdout
	autogen.Stderr = os.Stderr

	if err := autogen.Run(); err != nil {
		return "", "", err
	}
	configureArgs := []string{
		"--disable-asciidoc",
	}
	// If you're using M1 or later CPUs, homebrew installs under /opt/local as
	// opposed to /usr/local. We need to tell tor's configure about that.
	if info, err := os.Stat("/opt/homebrew"); err == nil && info.IsDir() {
		configureArgs = append(configureArgs, "--with-libevent-dir=/opt/homebrew/")
		configureArgs = append(configureArgs, "--with-openssl-dir=/opt/homebrew/opt/openssl@1.1/")
	}
	configure := exec.Command("./configure", configureArgs...)
	configure.Dir = tgtf
	configure.Stdout = os.Stdout
	configure.Stderr = os.Stderr

	if err := configure.Run(); err != nil {
		return "", "", err
	}
	// Retrieve the version of the current commit
	winconf, _ := ioutil.ReadFile(filepath.Join(tgtf, "src", "win32", "orconfig.h"))
	strver := regexp.MustCompile("define VERSION \"(.+)\"").FindSubmatch(winconf)[1]

	// Hook the make system and gather the needed sources
	maker := exec.Command("make", "--dry-run")
	maker.Dir = tgtf

	out, err := maker.CombinedOutput()
	if err != nil {
		fmt.Println(string(out))
		return "", "", err
	}
	deps := regexp.MustCompile("(?m)([a-z0-9_/-]+)\\.c").FindAllStringSubmatch(string(out), -1)

	// Wipe everything from the library that's non-essential
	files, err := ioutil.ReadDir(tgtf)
	if err != nil {
		return "", "", err
	}
	for _, file := range files {
		// Remove all folders apart from the sources
		if file.IsDir() {
			if file.Name() == "src" {
				continue
			}
			os.RemoveAll(filepath.Join(tgtf, file.Name()))
			continue
		}
		// Remove all files apart from the license
		if file.Name() == "LICENSE" {
			continue
		}
		os.Remove(filepath.Join(tgtf, file.Name()))
	}
	// Wipe all the sources from the library that are non-essential
	files, err = ioutil.ReadDir(filepath.Join(tgtf, "src"))
	if err != nil {
		return "", "", err
	}
	for _, file := range files {
		if file.IsDir() {
			if file.Name() == "app" || file.Name() == "core" || file.Name() == "ext" || file.Name() == "feature" || file.Name() == "lib" || file.Name() == "trunnel" || file.Name() == "win32" {
				continue
			}
			os.RemoveAll(filepath.Join(tgtf, "src", file.Name()))
			continue
		}
		os.Remove(filepath.Join(tgtf, "src", file.Name()))
	}
	// Wipe all the weird .Po files containing dummies
	if err := filepath.Walk(filepath.Join(tgtf, "src"),
		func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}
			if filepath.Base(path) == ".deps" {
				os.RemoveAll(path)
				return filepath.SkipDir
			}
			return nil
		},
	); err != nil {
		return "", "", err
	}
	// Fix the string compatibility source to load the correct code
	blob, _ := ioutil.ReadFile(filepath.Join(tgtf, "src", "lib", "string", "compat_string.c"))
	ioutil.WriteFile(filepath.Join(tgtf, "src", "lib", "string", "compat_string.c"), bytes.Replace(blob, []byte("strlcpy.c"), []byte("ext/strlcpy.c"), -1), 0644)

	// TarGeTFILTer
	tgtFilt := targetFilters[tgt]

	tmpl, err := template.New("").Parse(torTemplate)
	if err != nil {
		return "", "", err
	}
	for _, dep := range deps {
		// Skip any files not needed for the library
		if strings.HasPrefix(dep[1], "src/ext/tinytest") {
			continue
		}
		if strings.HasPrefix(dep[1], "src/test/") {
			continue
		}
		if strings.HasPrefix(dep[1], "src/tools/") {
			continue
		}
		// Skip the main tor entry point, we're wrapping a lib
		if strings.HasSuffix(dep[1], "tor_main") {
			continue
		}
		// The donna crypto library needs architecture specific linking
		if strings.HasSuffix(dep[1], "-c64") {
			for _, arch := range []string{"amd64", "arm64"} {
				gofile := strings.Replace(dep[1], "/", "_", -1) + "_" + arch + ".go"
				buff := new(bytes.Buffer)
				if err := tmpl.Execute(buff, map[string]string{
					"TargetFilter": tgtFilt,
					"File":         dep[1],
				}); err != nil {
					return "", "", err
				}
				ioutil.WriteFile(filepath.Join("libtor", tgt+"_tor_"+gofile), buff.Bytes(), 0644)
			}
			for _, arch := range []string{"386", "arm"} {
				gofile := strings.Replace(dep[1], "/", "_", -1) + "_" + arch + ".go"
				buff := new(bytes.Buffer)
				if err := tmpl.Execute(buff, map[string]string{
					"TargetFilter": tgtFilt,
					"File":         strings.Replace(dep[1], "-c64", "", -1),
				}); err != nil {
					return "", "", err
				}
				ioutil.WriteFile(filepath.Join("libtor", tgt+"_tor_"+gofile), buff.Bytes(), 0644)
			}
			continue
		}
		// Anything else gets wrapped directly
		gofile := strings.Replace(dep[1], "/", "_", -1) + ".go"
		buff := new(bytes.Buffer)
		if err := tmpl.Execute(buff, map[string]string{
			"TargetFilter": tgtFilt,
			"File":         dep[1],
		}); err != nil {
			return "", "", err
		}
		ioutil.WriteFile(filepath.Join("libtor", tgt+"_tor_"+gofile), buff.Bytes(), 0644)
	}
	tmpl, err = template.New("").Parse(torPreamble)
	if err != nil {
		return "", "", err
	}
	buff := new(bytes.Buffer)
	if err := tmpl.Execute(buff, map[string]string{
		"TargetFilter": tgtFilt,
		"Target":       tgt,
	}); err != nil {
		return "", "", err
	}
	ioutil.WriteFile(filepath.Join("libtor", tgt+"_tor_preamble.go"), buff.Bytes(), 0644)

	// Inject the configuration headers and ensure everything builds
	os.MkdirAll(filepath.Join("tor_config"), 0755)

	for _, arch := range []string{"", ".linux64", ".linux32", ".android64", ".android32", ".macos64", ".ios64"} {
		blob, _ := ioutil.ReadFile(filepath.Join("config", "tor", fmt.Sprintf("orconfig%s.h", arch)))
		tmpl, err := template.New("").Parse(string(blob))
		if err != nil {
			return "", "", err
		}
		buff := new(bytes.Buffer)
		if err := tmpl.Execute(buff, struct{ StrVer string }{string(strver)}); err != nil {
			return "", "", err
		}
		ioutil.WriteFile(filepath.Join("tor_config", fmt.Sprintf("orconfig%s.h", arch)), buff.Bytes(), 0644)
	}
	blob, _ = ioutil.ReadFile(filepath.Join("config", "tor", "micro-revision.i"))
	ioutil.WriteFile(filepath.Join("tor_config", "micro-revision.i"), blob, 0644)
	return string(strver), string(commit), nil
}

// torPreamble is the CGO preamble injected to configure the C compiler.
var torPreamble = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor

/*
#cgo CFLAGS: -I${SRCDIR}/../tor_config
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/tor
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/tor/src
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/tor/src/core/or
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/tor/src/ext
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/tor/src/ext/trunnel
#cgo CFLAGS: -I${SRCDIR}/../{{.Target}}/tor/src/feature/api

#cgo CFLAGS: -DED25519_CUSTOMRANDOM -DED25519_CUSTOMHASH -DED25519_SUFFIX=_donna

#cgo LDFLAGS: -lm
*/
import "C"
`

// torTemplate is the source file template used in Tor Go wrappers.
var torTemplate = `// go-libtor - Self-contained Tor from Go
// Copyright (c) 2018 Péter Szilágyi. All rights reserved.
// +build {{.TargetFilter}}

package libtor

/*
#define BUILDDIR ""

#include <../{{.File}}.c>
*/
import "C"
`
