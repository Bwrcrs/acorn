# ============================================================
#  Acorn test runner
#
#  Usage:
#      .\tests\run_tests.ps1            run all tests
#      .\tests\run_tests.ps1 -Update    regenerate expected output
#
#  Two kinds of test live under tests\:
#
#    cases\NAME.sql   run as `acorn.exe NAME.sql`
#                     expected output in cases\NAME.out
#
#    args\NAME.args   one command-line argument per line (may be empty)
#                     expected output in args\NAME.out
#                     used for flags: --ast / --tokens / -e / bad options
#
#    repl\NAME.repl   piped into acorn's stdin, so it runs the REPL
#                     expected output in repl\NAME.out
#                     used for meta-commands (.tables / .schema / .quit)
#                     and for the "accumulate until a semicolon" logic
#
#    disk\NAME.disk   a SEQUENCE of acorn invocations against ONE database
#                     file, each in a fresh temp directory, each its own
#                     process -- expected output in disk\NAME.out
#                     this is the only way to test that data really
#                     survives the process boundary
#
#                     Each line is one of:
#                       (blank)         ignored
#                       # ...           comment
#                       ARG|ARG|...     run acorn with these arguments
#                                       (a non-zero exit code is recorded
#                                        right after the output as `! exit N`
#                                        -- see ExitLine below)
#                       stdin:TEXT      run acorn with TEXT on stdin
#                                       (write \n where you want a newline)
#                       !raw KIND       overwrite test.db with a crafted file
#                                       KIND = empty | badsize | magic
#                                            | version | pagesize
#                                            | journalbad
#                                       (journalbad writes a corrupt
#                                        test.db-journal -- see New-RawDb)
#                       !bulk T|N|W     generate N rows of (i, 'WWW...') for a
#                                       table shaped (INT, TEXT), as ONE insert
#                                       statement, into bulk.sql -- then run it
#                                       (one statement, so the .out gets one
#                                        line instead of N)
#                       !pages N        assert test.db is exactly N pages
#                                       (echoed into the .out, and a hard
#                                        failure if it is not)
#                       !bytes N        assert test.db is exactly N bytes
#                                       -- for the crafted-file cases, where
#                                       "nothing was written" is the claim and
#                                       a byte count says it precisely
#                       !exists NAME    assert NAME is there in the work dir
#                       !missing NAME   assert NAME is NOT there
#                                       -- used for the rollback journal:
#                                       "committing is deleting the journal"
#                                       is the whole contract, so the presence
#                                       or absence of that one file IS the
#                                       assertion.
#                       !bg MS|SQL      start acorn in the BACKGROUND against
#                                       test.db, running SQL and then holding
#                                       the database open for MS milliseconds
#                                       (--hold).  We wait until it says it is
#                                       holding, so the next line is guaranteed
#                                       to run while it is still there.
#                       !wait           wait for that background process, then
#                                       print everything it said
#                                       (no exit code: Start-Process -PassThru
#                                        gives an object whose ExitCode is null
#                                        on PowerShell 5.1 -- measured, not
#                                        assumed. The foreground commands are
#                                        where exit codes are recorded, and
#                                        they are the ones a script sees.)
#                       !killbg         kill it without warning -- no
#                                       destructors, no flush, exactly like a
#                                       power cut or Task Manager.  Used to
#                                       prove that the lock goes away with the
#                                       process (the kernel releases it).
#                                       -- the whole point of phase 6 is that
#                                       TWO processes are in the database at
#                                       once, and that state cannot be reached
#                                       from a single linear script.
#
#  NOTE: keep this file pure ASCII. PowerShell 5.1 reads a .ps1 with no
#  BOM using the system ANSI codepage (GBK here), so Chinese characters
#  come out mangled and the script may not even parse. Chinese lives in
#  the .out / .args data files, which we read as UTF-8 explicitly.
#  (Same trap as build.bat -- learned in the Nova project.)
#
#  NOTE: we do NOT use `$out = & $exe ...` to capture output. PowerShell
#  would decode the child's bytes using [Console]::OutputEncoding, which
#  is GBK on this machine, and every Chinese character would turn to
#  mojibake. Instead we let cmd.exe do a byte-level redirect into a file
#  and read that file back as UTF-8. Redirecting both streams into ONE
#  file also preserves their relative order -- acorn flushes stdout
#  before writing to stderr precisely so this ordering is meaningful.
# ============================================================

param([switch]$Update)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$exe      = Join-Path $root 'acorn.exe'
$casesDir = Join-Path $PSScriptRoot 'cases'
$argsDir  = Join-Path $PSScriptRoot 'args'
$replDir  = Join-Path $PSScriptRoot 'repl'
$diskDir  = Join-Path $PSScriptRoot 'disk'
$diskWork = Join-Path $PSScriptRoot '_disk.tmp'
$actual   = Join-Path $PSScriptRoot '_actual.tmp'

if (-not (Test-Path $exe)) {
    Write-Host "[FAIL] not found: $exe"
    Write-Host "       run build.bat first"
    exit 1
}

# ---- UTF-8 without BOM ----
$utf8NoBom = New-Object System.Text.UTF8Encoding $false

function Read-Utf8([string]$path) {
    return [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
}

function Write-Utf8([string]$path, [string]$text) {
    # The CRT turns every "\n" the program writes into "\r\n" because
    # stdout is a text-mode stream on Windows. Normalise back to "\n"
    # so the .out files are readable and diffable.
    $normalised = ($text -replace "`r`n", "`n")
    [System.IO.File]::WriteAllText($path, $normalised, $utf8NoBom)
}

function Normalise([string]$text) {
    $t = $text -replace "`r`n", "`n"
    return $t.TrimEnd("`n")
}

# Read a file that another process may still have open for writing.
#
# [Console]::WriteLine and File.ReadAllText both open with FileShare.Read,
# which CONFLICTS with a live writer -- and a live writer is exactly what
# we are looking at here. Opening with FileShare.ReadWrite lets us peek at
# the background process's output while it is still running, which is how
# we know it has taken the lock. (Learned the hard way: the first attempt
# threw "being used by another process" sixteen times in a row.)
function Read-Shared([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
    } catch {
        return ''
    }
    $reader = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
    $text = $reader.ReadToEnd()
    $reader.Close()
    return $text
}

# Scratch files never outlive the run. If one is left behind it shows up
# as an untracked file and the next person has to work out whether it
# matters.
function Remove-Scratch {
    if (Test-Path $actual)   { Remove-Item $actual -Force }
    if (Test-Path $diskWork) { Remove-Item $diskWork -Recurse -Force }
}

# Run acorn with the given argument list, return everything it printed.
# $inputFile, when given, is piped into acorn's stdin (that is how the
# REPL gets driven in a test).
#
# We always `cd` into a known directory first. Without that, a test that
# names a file would resolve it against whatever directory the person
# happened to run the script from -- so the suite would pass on one
# machine and fail on another for no interesting reason.
function Invoke-Acorn([string[]]$argList, [string]$inputFile, [string]$dir) {
    if (Test-Path $actual) { Remove-Item $actual -Force }

    # Quote every argument: SQL contains spaces, parentheses and
    # semicolons, and cmd.exe would otherwise split it apart.
    $quoted = ($argList | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '
    $redirectIn = ''
    if ($inputFile) { $redirectIn = '< "{0}" ' -f $inputFile }
    $line = 'cd /d "{0}" && "{1}" {2} {3}> "{4}" 2>&1' -f $dir, $exe, $quoted, $redirectIn, $actual

    & cmd /c $line | Out-Null   # cmd's own stdout is empty; output went to the file

    # cmd /c hands back the child's exit status, so this IS acorn's.
    # Recorded by the caller as "! exit N" -- but only when it is not 0,
    # because 0 is the boring case and 114 baselines full of "! exit 0"
    # would bury the few lines that mean something.
    $script:lastExit = $LASTEXITCODE

    if (-not (Test-Path $actual)) { return '' }
    return Read-Utf8 $actual
}

# How a non-zero exit gets written into the .out. It is an assertion, so
# it uses the same "!" shape as !pages and !missing.
#
# Why the exit code is worth recording: it is the only part of this that a
# SCRIPT can see. A person reads the "error: ..." line and knows what
# happened; a script reads the exit code:
#   1 = the SQL is wrong (retrying will not help)
#   2 = the command line or the environment is wrong; one case is "the
#       database is open in another process" (retrying later might help)
# Without that split, a caller has to regex the Chinese error message --
# which treats the wording as an API, so rephrasing a hint quietly breaks
# a caller.
function ExitLine([int]$code) {
    if ($code -eq 0) { return '' }
    return ('! exit {0}' -f $code)
}

# Same thing, but for output that is being built up line by line: an empty
# ExitLine must add NOTHING. AppendLine('') would leave a blank line in the
# baseline after every successful command.
function AppendExit($sb, [int]$code) {
    $line = ExitLine $code
    if ($line.Length -gt 0) { [void]$sb.AppendLine($line) }
}

# ---- disk tests ----------------------------------------------------------
#
# A .disk file is a script over one database file. Every line is its own
# acorn process, so anything that only lives in RAM is gone by the next
# line -- which is exactly the property we want to pin down.

# Overwrite test.db with a deliberately broken file. These are generated
# rather than checked in as binaries: a 4096-byte blob in the repo tells
# you nothing, while four lines of PowerShell say exactly which byte is
# wrong and why the program is supposed to notice.
function New-RawDb([string]$path, [string]$kind) {
    $pageSize = [BitConverter]::GetBytes([uint32]4096)

    switch ($kind) {
        'journalbad' {
            # A rollback journal whose header does not check out. The magic
            # is right but the rest is noise, so the header checksum (a plain
            # byte sum) cannot match either.
            #
            # The program must refuse to touch the database entirely. Guessing
            # here would mean accepting a half-written transaction as complete
            # -- real corruption. The .out records the refusal, and the
            # !bytes assertion right after proves test.db was not written.
            $j = New-Object byte[] 4128
            [System.Text.Encoding]::ASCII.GetBytes('ACORJ').CopyTo($j, 0)
            $j[5] = 1
            for ($i = 16; $i -lt 4128; $i++) { $j[$i] = 0xA5 }
            [System.IO.File]::WriteAllBytes($path + '-journal', $j)
        }
        'empty' {
            [System.IO.File]::WriteAllBytes($path, (New-Object byte[] 0))
        }
        'badsize' {
            # Not a whole number of pages: 100 bytes.
            $b = New-Object byte[] 100
            for ($i = 0; $i -lt 100; $i++) { $b[$i] = 0x78 }
            [System.IO.File]::WriteAllBytes($path, $b)
        }
        'magic' {
            # Right size, wrong opening bytes.
            $b = New-Object byte[] 4096
            [System.Text.Encoding]::ASCII.GetBytes('NOPE!').CopyTo($b, 8)
            [System.IO.File]::WriteAllBytes($path, $b)
        }
        'version' {
            # A real Acorn file -- from the future.
            $b = New-Object byte[] 4096
            [System.Text.Encoding]::ASCII.GetBytes('ACORN').CopyTo($b, 8)
            $b[13] = 99
            $pageSize.CopyTo($b, 16)
            [System.IO.File]::WriteAllBytes($path, $b)
        }
        'pagesize' {
            # Says 2048. Every offset after this would be wrong, and it
            # would not crash -- it would just read garbage.
            #
            # The version byte has to be the CURRENT one, or the version
            # check fires first and this case silently stops testing the
            # page-size branch at all. Keep it in step with
            # ACORN_FORMAT_VERSION in src/page.hpp.
            $b = New-Object byte[] 4096
            [System.Text.Encoding]::ASCII.GetBytes('ACORN').CopyTo($b, 8)
            $b[13] = 2
            [BitConverter]::GetBytes([uint32]2048).CopyTo($b, 16)
            [System.IO.File]::WriteAllBytes($path, $b)
        }
        default { throw ("unknown !raw kind: " + $kind) }
    }
}

# For the .out file: show what was run the way a person would have typed it.
function Show-Args([string[]]$argList) {
    return (($argList | ForEach-Object {
        if ($_ -match '[\s;()]') { '"' + $_ + '"' } else { $_ }
    }) -join ' ')
}

# Returns the text to record, and sets $script:diskHardFail if a !pages
# assertion did not hold.
function Invoke-Disk([string]$path) {
    if (Test-Path $diskWork) { Remove-Item $diskWork -Recurse -Force }
    New-Item -ItemType Directory -Path $diskWork | Out-Null
    $script:diskHardFail = $false
    $script:bgProc = $null
    $script:bgOut = ''
    $script:bgErr = ''

    $dbPath = Join-Path $diskWork 'test.db'
    $out    = New-Object System.Text.StringBuilder

    $lines = [System.IO.File]::ReadAllLines($path, [System.Text.Encoding]::UTF8)

    foreach ($line in $lines) {
        $t = $line.Trim()
        if ($t.Length -eq 0) { continue }
        if ($t.StartsWith('#')) { continue }

        if ($t.StartsWith('!raw ')) {
            $kind = $t.Substring(5).Trim()
            New-RawDb $dbPath $kind
            [void]$out.AppendLine('! raw ' + $kind)
            continue
        }

        if ($t.StartsWith('!bulk ')) {
            # !bulk TABLE|ROWS|WIDTH
            $spec  = $t.Substring(6).Trim() -split '\|'
            $table = $spec[0]
            $rows  = [int]$spec[1]
            $width = [int]$spec[2]

            # Built as ONE statement with N tuples on purpose. N separate
            # INSERTs would print "inserted 1 row" N times and bury the
            # assertion we actually care about under N lines of noise.
            $pad = 'x' * $width
            $sb  = New-Object System.Text.StringBuilder
            [void]$sb.Append('INSERT INTO ' + $table + ' VALUES ')
            for ($i = 1; $i -le $rows; $i++) {
                if ($i -gt 1) { [void]$sb.Append(',') }
                [void]$sb.Append("($i,'$pad')")
            }
            [void]$sb.Append(';')

            $bulkFile = Join-Path $diskWork 'bulk.sql'
            [System.IO.File]::WriteAllText($bulkFile, $sb.ToString(), $utf8NoBom)

            [void]$out.AppendLine(('! bulk {0}: {1} rows x {2} byte text -> bulk.sql' -f $table, $rows, $width))
            [void]$out.AppendLine('$ -d test.db bulk.sql')
            [void]$out.Append((Invoke-Acorn @('-d', 'test.db', 'bulk.sql') '' $diskWork))
            continue
        }

        if ($t.StartsWith('!pages ')) {
            $want = [int]$t.Substring(7).Trim()
            if (-not (Test-Path $dbPath)) {
                [void]$out.AppendLine('! pages ' + $want + '  ->  test.db does not exist')
                $script:diskHardFail = $true
                continue
            }
            $size = (Get-Item $dbPath).Length
            $got  = [int]($size / 4096)
            [void]$out.AppendLine(('! pages {0}  ->  file is {1} bytes = {2} pages' -f $want, $size, $got))
            if ($got -ne $want -or ($size % 4096) -ne 0) {
                [void]$out.AppendLine('    ^^^ expected ' + $want + ' pages')
                $script:diskHardFail = $true
            }
            continue
        }

        if ($t.StartsWith('!bytes ')) {
            $want = [int]$t.Substring(7).Trim()
            if (-not (Test-Path $dbPath)) {
                [void]$out.AppendLine('! bytes ' + $want + '  ->  test.db does not exist')
                $script:diskHardFail = $true
                continue
            }
            $size = (Get-Item $dbPath).Length
            [void]$out.AppendLine(('! bytes {0}  ->  file is {1} bytes' -f $want, $size))
            if ($size -ne $want) {
                [void]$out.AppendLine('    ^^^ expected ' + $want + ' bytes')
                $script:diskHardFail = $true
            }
            continue
        }

        if ($t.StartsWith('!exists ') -or $t.StartsWith('!missing ')) {
            $wantThere = $t.StartsWith('!exists ')
            if ($wantThere) {
                $verb = 'exists'
                $name = $t.Substring(8).Trim()
            } else {
                $verb = 'missing'
                $name = $t.Substring(9).Trim()
            }
            $there = Test-Path (Join-Path $diskWork $name)
            if ($there) { $state = 'it is there' } else { $state = 'it is not there' }
            [void]$out.AppendLine('! ' + $verb + ' ' + $name + '  ->  ' + $state)
            if ($there -ne $wantThere) {
                if ($wantThere) { $expect = 'there' } else { $expect = 'gone' }
                [void]$out.AppendLine('    ^^^ expected it to be ' + $expect)
                $script:diskHardFail = $true
            }
            continue
        }

        if ($t.StartsWith('!bg ')) {
            if ($null -ne $script:bgProc) {
                [void]$out.AppendLine('    ^^^ a background acorn is already running')
                $script:diskHardFail = $true
                continue
            }

            # !bg HOLD_MS|SQL
            $spec = $t.Substring(4).Trim()
            $bar  = $spec.IndexOf('|')
            if ($bar -lt 0) {
                [void]$out.AppendLine('    ^^^ !bg wants HOLD_MS|SQL')
                $script:diskHardFail = $true
                continue
            }
            $hold = $spec.Substring(0, $bar).Trim()
            $sql  = $spec.Substring($bar + 1)

            # The SQL goes in a FILE, not on the command line.
            #
            # Start-Process joins its argument array with spaces and does
            # not quote anything, so "-e SELECT * FROM t;" arrives as six
            # separate arguments and acorn sees a statement called
            # "SELECT". A file has no such problem, and it also keeps the
            # command line pure ASCII (SQL with Chinese in it is fine).
            $bgSql = Join-Path $diskWork '_bg.sql'
            [System.IO.File]::WriteAllText($bgSql, $sql + "`n", $utf8NoBom)

            $bgOut = Join-Path $diskWork '_bg.out'
            $bgErr = Join-Path $diskWork '_bg.err'
            $script:bgOut = $bgOut
            $script:bgErr = $bgErr

            $script:bgProc = Start-Process -FilePath $exe `
                -ArgumentList @('-d', 'test.db', ('--hold=' + $hold), '_bg.sql') `
                -WorkingDirectory $diskWork -PassThru -WindowStyle Hidden `
                -RedirectStandardOutput $bgOut -RedirectStandardError $bgErr

            [void]$out.AppendLine(('! bg --hold={0}  (SQL: {1})' -f $hold, $sql))

            # Wait for it to say it is holding the database.
            #
            # Not a fixed sleep: a test that says "500 ms ought to be
            # enough" is a test that passes on this machine and fails on a
            # busy one, and a test that sometimes fails is worse than no
            # test at all -- it teaches people to ignore failures.
            # --hold prints a marker and flushes, so we can just wait for
            # that line to show up.
            $deadline = (Get-Date).AddSeconds(15)
            $ready = $false
            while ((Get-Date) -lt $deadline) {
                if ((Read-Shared $bgOut).Contains('[--hold]')) { $ready = $true; break }

                # If it is gone, stop waiting for a marker that is never
                # coming. The check is only a shortcut for the deadline --
                # so it must not be able to turn a live process into a
                # failure. WaitForExit makes it authoritative (a process
                # that is alive blocks here until it really ends, and by
                # then the marker IS in the file, because acorn flushes it
                # before sleeping), and we look once more afterwards.
                if ($script:bgProc.HasExited) {
                    $script:bgProc.WaitForExit(15000) | Out-Null
                    if ((Read-Shared $bgOut).Contains('[--hold]')) { $ready = $true }
                    break
                }
                Start-Sleep -Milliseconds 30
            }
            if ($ready) {
                [void]$out.AppendLine('! bg is up: it holds the database now')
            } else {
                # It died instead of getting ready. Say so, and print what
                # it said while dying: "no marker" on its own sends the
                # next reader hunting through acorn for a bug that is
                # usually just a typo in the SQL above.
                [void]$out.AppendLine('    ^^^ the background acorn went away without holding the database')
                [void]$out.AppendLine('    ^^^ what it said:')
                [void]$out.Append((Read-Shared $bgOut))
                $bgErrText = Read-Shared $bgErr
                if ($bgErrText.Trim().Length -gt 0) { [void]$out.Append($bgErrText) }
                $script:diskHardFail = $true
                $script:bgProc = $null
            }
            continue
        }

        if ($t.StartsWith('!wait')) {
            if ($null -eq $script:bgProc) {
                [void]$out.AppendLine('    ^^^ !wait with no background acorn')
                $script:diskHardFail = $true
                continue
            }
            $script:bgProc.WaitForExit(20000) | Out-Null
            $script:bgProc = $null
            [void]$out.AppendLine('! wait -> it is gone')
            [void]$out.AppendLine('! --- its stdout ---')
            [void]$out.Append((Read-Shared $script:bgOut))
            $bgErrText = Read-Shared $script:bgErr
            if ($bgErrText.Trim().Length -gt 0) {
                [void]$out.AppendLine('! --- its stderr ---')
                [void]$out.Append($bgErrText)
            }
            continue
        }

        if ($t.StartsWith('!killbg')) {
            if ($null -eq $script:bgProc) {
                [void]$out.AppendLine('    ^^^ !killbg with no background acorn')
                $script:diskHardFail = $true
                continue
            }

            # Killing something that has already died proves nothing:
            # "the lock went away with the process" only means something
            # if there WAS a process holding it. So an already-gone acorn
            # is a hard failure, not a shrug.
            #
            # (Stop-Process also *throws* when the pid is gone, and with
            # $ErrorActionPreference = 'Stop' that ends the whole run with
            # a PowerShell stack trace instead of naming the test -- which
            # is how this check got written.)
            if ($script:bgProc.HasExited) {
                [void]$out.AppendLine('    ^^^ the background acorn was already gone -- nothing to kill')
                $script:diskHardFail = $true
            } else {
                Stop-Process -Id $script:bgProc.Id -Force -ErrorAction SilentlyContinue
            }
            $script:bgProc.WaitForExit(10000) | Out-Null
            $script:bgProc = $null
            [void]$out.AppendLine('! killbg -> killed. No destructors ran, no buffers were flushed.')
            continue
        }

        if ($t.StartsWith('stdin:')) {
            # Always against the disk database -- this is a .disk file, and
            # the whole point is that the database outlives the process.
            # (A stdin run without -d would quietly use an in-memory
            # database and every assertion here would be meaningless.)
            $text = $t.Substring(6).Replace('\n', "`n")
            $stdinFile = Join-Path $diskWork '_stdin.tmp'
            [System.IO.File]::WriteAllText($stdinFile, $text, $utf8NoBom)
            # Record it as: the command that ran, and what was typed at it.
            # Not as a shell pipeline -- we are not running one, and a made-up
            # command in a test baseline is a lie the next reader has to
            # disprove. (\n here is literal: this is one line of the .disk file.)
            [void]$out.AppendLine('$ acorn -d test.db      # stdin: ' + $t.Substring(6))
            [void]$out.Append((Invoke-Acorn @('-d', 'test.db') $stdinFile $diskWork))
            AppendExit $out $script:lastExit
            continue
        }

        $argList = @($t -split '\|')
        [void]$out.AppendLine('$ ' + (Show-Args $argList))
        [void]$out.Append((Invoke-Acorn $argList '' $diskWork))
        AppendExit $out $script:lastExit
    }

    # A .disk file that leaves a background process running is a bug in
    # the test: the work directory is about to be deleted underneath it,
    # and the process would keep holding a lock on a file that no longer
    # has a name. Kill it and say so.
    if ($null -ne $script:bgProc) {
        if (-not $script:bgProc.HasExited) {
            Stop-Process -Id $script:bgProc.Id -Force -ErrorAction SilentlyContinue
        }
        $script:bgProc.WaitForExit(10000) | Out-Null
        $script:bgProc = $null
        [void]$out.AppendLine('    ^^^ this test left a background acorn running')
        $script:diskHardFail = $true
    }

    return $out.ToString()
}


# ---- collect the tests ----
$tests = @()

if (Test-Path $casesDir) {
    foreach ($f in (Get-ChildItem -Path $casesDir -Filter *.sql | Sort-Object Name)) {
        $tests += [pscustomobject]@{
            Name     = 'cases\' + $f.Name
            Kind     = 'run'
            ArgList  = @($f.FullName)
            Input    = ''
            File     = ''
            Expected = [System.IO.Path]::ChangeExtension($f.FullName, '.out')
        }
    }
}

if (Test-Path $replDir) {
    foreach ($f in (Get-ChildItem -Path $replDir -Filter *.repl | Sort-Object Name)) {
        $tests += [pscustomobject]@{
            Name     = 'repl\' + $f.Name
            Kind     = 'run'
            ArgList  = @()
            Input    = $f.FullName
            File     = ''
            Expected = [System.IO.Path]::ChangeExtension($f.FullName, '.out')
        }
    }
}

if (Test-Path $argsDir) {
    foreach ($f in (Get-ChildItem -Path $argsDir -Filter *.args | Sort-Object Name)) {
        # One argument per line. An empty file means "no arguments".
        $lines = [System.IO.File]::ReadAllLines($f.FullName, [System.Text.Encoding]::UTF8)
        $list = @($lines | Where-Object { $_.Length -gt 0 })
        $tests += [pscustomobject]@{
            Name     = 'args\' + $f.Name
            Kind     = 'run'
            ArgList  = $list
            Input    = ''
            File     = ''
            Expected = [System.IO.Path]::ChangeExtension($f.FullName, '.out')
        }
    }
}

if (Test-Path $diskDir) {
    foreach ($f in (Get-ChildItem -Path $diskDir -Filter *.disk | Sort-Object Name)) {
        $tests += [pscustomobject]@{
            Name     = 'disk\' + $f.Name
            Kind     = 'disk'
            ArgList  = @()
            Input    = ''
            File     = $f.FullName
            Expected = [System.IO.Path]::ChangeExtension($f.FullName, '.out')
        }
    }
}

if ($tests.Count -eq 0) {
    Write-Host '[FAIL] no test cases found'
    exit 1
}

# ---- run ----
$passed = 0
$failed = @()

foreach ($t in $tests) {
    if ($t.Kind -eq 'disk') {
        $got = Normalise (Invoke-Disk $t.File)
        $hardFail = $script:diskHardFail
    } else {
        $got = Normalise (Invoke-Acorn $t.ArgList $t.Input $root)
        $e = ExitLine $script:lastExit
        if ($e.Length -gt 0) { $got = $got + "`n" + $e }
        $hardFail = $false
    }

    if ($Update) {
        Write-Utf8 $t.Expected $got
        if ($hardFail) {
            Write-Host ("  updated  " + $t.Name + "   (but a !pages check failed!)")
        } else {
            Write-Host ("  updated  " + $t.Name)
        }
        continue
    }

    if (-not (Test-Path $t.Expected)) {
        $failed += $t.Name
        Write-Host ("  MISSING  " + $t.Name + "   (no .out file -- run with -Update)")
        continue
    }

    $want = Normalise (Read-Utf8 $t.Expected)

    if ($hardFail) {
        $failed += $t.Name
        Write-Host ("  FAILED   " + $t.Name + "   (page count assertion)")
    } elseif ($got -ceq $want) {
        $passed++
        Write-Host ("  ok       " + $t.Name)
    } else {
        $failed += $t.Name
        Write-Host ("  FAILED   " + $t.Name)
    }
}

if ($Update) {
    Remove-Scratch
    Write-Host ""
    Write-Host ("[OK] regenerated " + $tests.Count + " expected-output files")
    Write-Host "     Read the diffs before committing -- a .out file that"
    Write-Host "     regenerates happily can still be recording a bug."
    exit 0
}

Write-Host ""

if ($failed.Count -gt 0) {
    Remove-Scratch
    Write-Host ("[FAIL] " + $passed + " passed, " + $failed.Count + " failed")
    Write-Host ""
    foreach ($name in $failed) { Write-Host ("  " + $name) }
    Write-Host ""
    Write-Host "To see what actually happened, run the case by hand:"
    Write-Host "    .\acorn.exe tests\cases\<name>.sql"
    Write-Host "For a disk test, run one line at a time against one file:"
    Write-Host "    .\acorn.exe -d scratch.db -e ""..."""
    exit 1
}

Remove-Scratch

Write-Host ("[OK] all " + $passed + " tests passed")
exit 0
