[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [int[]]$Counts,
    [int]$WarmupSeconds = 60,
    [int]$SampleSeconds = 60,
    [ValidateRange(15, 120)]
    [int]$Fps = 120,
    [ValidateRange(0, 63)]
    [int]$MonitorIndex = 1,
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$PresentMonPath,
    [ValidateRange(0, 8)]
    [int]$Threads = 0
)

$ErrorActionPreference = 'Stop'
if ($WarmupSeconds -lt 0) { throw 'WarmupSeconds must be zero or greater.' }
if ($SampleSeconds -lt 1) { throw 'SampleSeconds must be at least one.' }
if ($Counts.Count -eq 0) { throw 'Counts must contain at least one bird count.' }
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Executable not found: $Executable" }
foreach ($count in $Counts) {
    if ($count -lt 0) { throw "Counts must contain nonnegative bird counts; received $count." }
}
Add-Type -AssemblyName System.Windows.Forms
if ($MonitorIndex -ge [System.Windows.Forms.Screen]::AllScreens.Length) {
    throw "Requested monitor $MonitorIndex is unavailable; refusing to fall back to the screen in use."
}

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$utf8NoBom = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false

function Quote-WindowsArgument([string]$Value) {
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append([char]34)
    $backslashCount = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]92) {
            $backslashCount++
            continue
        }
        if ($character -eq [char]34) {
            for ($i = 0; $i -lt (2 * $backslashCount + 1); $i++) { [void]$builder.Append([char]92) }
            [void]$builder.Append([char]34)
            $backslashCount = 0
            continue
        }
        for ($i = 0; $i -lt $backslashCount; $i++) { [void]$builder.Append([char]92) }
        $backslashCount = 0
        [void]$builder.Append($character)
    }
    for ($i = 0; $i -lt (2 * $backslashCount); $i++) { [void]$builder.Append([char]92) }
    [void]$builder.Append([char]34)
    return $builder.ToString()
}

function Convert-InvariantDouble([object]$Value) {
    $parsed = 0.0
    $text = [string]$Value
    $style = [Globalization.NumberStyles]::Float
    $culture = [Globalization.CultureInfo]::InvariantCulture
    if ([double]::TryParse($text, $style, $culture, [ref]$parsed)) { return $parsed }
    return [double]::NaN
}


$settingsText = @'
# Reynolds Boids Native full default quality profile; the runner changes only count and fps.
count={0}
size=6.58
speed=87
speedError=0.07
separation=1.38
alignment=1
cohesion=0.65
edge=0.175
trails=1
trailDuration=0.28
color=#84D8E0
brightness=1.31
contrast=1.2
randomColor=0
avoidTaskbar=1
marginLeft=0
marginRight=0
marginTop=0
marginBottom=0
renderScale=1
fps={1}
trailHz=30
monitor={2}
span=0
'@

$results = New-Object 'System.Collections.Generic.List[object]'
$presentMonResolvedPath = $null
$presentMonUnavailableReason = $null
if (-not [string]::IsNullOrWhiteSpace($PresentMonPath)) {
    if (Test-Path -LiteralPath $PresentMonPath -PathType Leaf) {
        $presentMonResolvedPath = (Resolve-Path -LiteralPath $PresentMonPath).Path
    } else {
        $presentMonUnavailableReason = "PresentMon executable was not found: $PresentMonPath"
    }
}

Write-Host "Testing monitor index $MonitorIndex. Threads: $Threads (0=auto). Application submission cap: $Fps FPS; this is not a claim of actual display FPS."

for ($runIndex = 0; $runIndex -lt $Counts.Count; $runIndex++) {
    $count = [int]$Counts[$runIndex]
    $fillingMinimumSeconds = [int][Math]::Ceiling($count / 450.0) + 30
    $effectiveWarmup = [Math]::Max($WarmupSeconds, $fillingMinimumSeconds)
    $runSeconds = [long]$effectiveWarmup + [long]$SampleSeconds + 1L
    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff', [Globalization.CultureInfo]::InvariantCulture)
    $runId = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $runName = "count-$count-$timestamp-$runId"
    $runDirectory = Join-Path $outputRoot $runName
    New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null

    $settingsDirectory = Join-Path $runDirectory 'ReynoldsBoids'
    New-Item -ItemType Directory -Force -Path $settingsDirectory | Out-Null
    $settingsPath = Join-Path $settingsDirectory 'settings.ini'
    $settingsContents = $settingsText -f $count, $Fps, $MonitorIndex
    [System.IO.File]::WriteAllText($settingsPath, $settingsContents, $utf8NoBom)

    $csvPath = Join-Path $runDirectory 'run.csv'
    $stdoutPath = Join-Path $runDirectory 'application.stdout.txt'
    $stderrPath = Join-Path $runDirectory 'application.stderr.txt'
    $metadataPath = Join-Path $runDirectory 'command-metadata.json'
    $presentMonCsvPath = Join-Path $runDirectory 'presentmon.raw.csv'
    $presentMonStdoutPath = Join-Path $runDirectory 'presentmon.stdout.txt'
    $presentMonStderrPath = Join-Path $runDirectory 'presentmon.stderr.txt'

    $applicationArguments = @('--monitor', [string]$MonitorIndex, '--count', [string]$count, '--threads', [string]$Threads, '--seconds', [string]$runSeconds, '--log', $csvPath)
    $applicationArgumentText = (@($applicationArguments | ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join ' ')
    $applicationCommand = (Quote-WindowsArgument $executablePath) + ' ' + $applicationArgumentText
    $metadata = [ordered]@{
        status = 'running'
        started_utc = [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
        run_directory = $runDirectory
        executable = $executablePath
        command_line = $applicationCommand
        environment = [ordered]@{ LOCALAPPDATA = $runDirectory }
        requested_count_birds = $count
        requested_threads = $Threads
        thread_policy = $(if ($Threads -eq 0) { '0_auto_hardware_concurrency_max_8' } elseif ($Threads -eq 1) { '1_serial' } else { "${Threads}_explicit_concurrency_limit" })
        fixed_simulation_seed = 20261022
        requested_warmup_seconds = $WarmupSeconds
        effective_warmup_seconds = $effectiveWarmup
        filling_minimum_seconds = $fillingMinimumSeconds
        sample_seconds = $SampleSeconds
        application_run_seconds = $runSeconds
        requested_application_fps_cap = $Fps
        fps_cap_note = 'Application submission cap only. 120 FPS is the throughput ceiling test; -Fps 60 is the capped baseline. No display FPS is claimed.'
        settings_file = $settingsPath
        settings = [ordered]@{
            count_birds = $count
            size_px = 6.58
            speed_px_per_s = 87
            speed_error_fraction = 0.07
            perception_radius_px = 58
            separation_radius_px = 15
            separation_weight = 1.38
            alignment_weight = 1
            cohesion_weight = 0.65
            edge_turn_fraction = 0.175
            trails_enabled = $true
            trail_duration_seconds = 0.28
            color_rgb_hex = '#84D8E0'
            brightness_multiplier = 1.31
            contrast_multiplier = 1.2
            random_color_enabled = $false
            avoid_taskbar = $true
            margins_px = [ordered]@{ left = 0; right = 0; top = 0; bottom = 0 }
            render_scale = 1
            fps_cap = $Fps
            trail_sample_hz = 30
            monitor_index = $MonitorIndex
            span_virtual_desktop = $false
        }
        seed_policy = 'Uses the native application fixed Simulation seed; no reset/restore/population injection or special-case behavior.'
        csv_log = $csvPath
        log_flush_behavior = 'buffered_capacity_measurement'
        log_flush_note = 'The header is flushed before sampling; per-frame rows and event comments are buffered and flushed/closed at run exit. Formatting and buffered file I/O still add measurement overhead; logging is not zero-overhead.'
        slow_frames_retained = $true
        interval_analysis_note = 'Raw CSV is retained unchanged. Any later submit_interval_ms analysis must exclude zero intervals; no slow frame rows are removed.'
        minimum_sample_rows = [Math]::Max(30, $SampleSeconds)
        minimum_sample_time_coverage_fraction = 0.9
        process_id = $null
        process_exit_code = $null
        process_peak_working_set_bytes = $null
        process_peak_working_set_mib = $null
        presentmon = [ordered]@{
            status = $(if ($null -eq $presentMonResolvedPath -and [string]::IsNullOrWhiteSpace($PresentMonPath)) { 'not_requested' } elseif ($null -ne $presentMonUnavailableReason) { 'unavailable' } else { 'not_started' })
            executable = $presentMonResolvedPath
            csv = $presentMonCsvPath
            detail = $presentMonUnavailableReason
            raw_stdout = $presentMonStdoutPath
            raw_stderr = $presentMonStderrPath
        }
        sample = $null
        automatic_pause_events = @()
        invalid_reasons = @()
        completed_utc = $null
    }
    [System.IO.File]::WriteAllText($metadataPath, ($metadata | ConvertTo-Json -Depth 10), $utf8NoBom)

    $app = $null
    $presentMonProcess = $null
    $presentMonStarted = $false
    $previousLocalAppData = [Environment]::GetEnvironmentVariable('LOCALAPPDATA', 'Process')
    $startFailure = $null
    $runStopwatch = [Diagnostics.Stopwatch]::StartNew()
    try {
        [Environment]::SetEnvironmentVariable('LOCALAPPDATA', $runDirectory, 'Process')
        $startArguments = @{
            FilePath = $executablePath
            ArgumentList = $applicationArgumentText
            WorkingDirectory = $runDirectory
            PassThru = $true
            WindowStyle = 'Normal'
            RedirectStandardOutput = $stdoutPath
            RedirectStandardError = $stderrPath
        }
        $app = Start-Process @startArguments
        $null = $app.Handle # Retain the process handle so ExitCode remains available after exit.
    } catch {
        $startFailure = $_.Exception.Message
    } finally {
        [Environment]::SetEnvironmentVariable('LOCALAPPDATA', $previousLocalAppData, 'Process')
    }

    if ($null -ne $app) {
        $metadata.process_id = $app.Id
        while ($runStopwatch.Elapsed.TotalSeconds -lt $effectiveWarmup -and -not $app.HasExited) {
            [void]$app.WaitForExit(200)
            $app.Refresh()
        }

        if ($presentMonResolvedPath -and $null -eq $presentMonUnavailableReason -and -not $app.HasExited) {
            $presentMonSeconds = $SampleSeconds + 5
            $presentMonArguments = @(
                '--process_id', [string]$app.Id,
                '--timed', [string]$presentMonSeconds,
                '--terminate_after_timed',
                '--output_file', $presentMonCsvPath,
                '--v2_metrics',
                '--no_track_input',
                '--write_display_metadata'
            )
            $presentMonArgumentText = (@($presentMonArguments | ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join ' ')
            try {
                $presentMonStartArguments = @{
                    FilePath = $presentMonResolvedPath
                    ArgumentList = $presentMonArgumentText
                    WorkingDirectory = $runDirectory
                    PassThru = $true
                    WindowStyle = 'Hidden'
                    RedirectStandardOutput = $presentMonStdoutPath
                    RedirectStandardError = $presentMonStderrPath
                }
                $presentMonProcess = Start-Process @presentMonStartArguments
                $presentMonStarted = $true
                $metadata.presentmon.status = 'running'
                $metadata.presentmon.command_line = (Quote-WindowsArgument $presentMonResolvedPath) + ' ' + $presentMonArgumentText
                $metadata.presentmon.process_id = $presentMonProcess.Id
                $metadata.presentmon.sample_seconds = $presentMonSeconds
            } catch {
                $presentMonUnavailableReason = "Could not start PresentMon: $($_.Exception.Message)"
                $metadata.presentmon.status = 'unavailable'
                $metadata.presentmon.detail = $presentMonUnavailableReason
            }
        } elseif ($presentMonResolvedPath -and $null -eq $presentMonUnavailableReason) {
            $metadata.presentmon.status = 'unavailable_target_exited_before_presentmon_start'
            $metadata.presentmon.detail = 'Target process exited before its effective warmup completed.'
        } elseif ($null -ne $presentMonUnavailableReason) {
            $metadata.presentmon.status = 'unavailable'
            $metadata.presentmon.detail = $presentMonUnavailableReason
        }

        while (-not $app.WaitForExit(1000)) {
            if ($runStopwatch.Elapsed.TotalSeconds -gt ($runSeconds + 30)) {
                Stop-Process -Id $app.Id -Force
                $metadata.watchdog_terminated = $true
                break
            }
        }
        $app.WaitForExit()
        $metadata.process_exit_code = $app.ExitCode
        try {
            $peakWorkingSet = [long]$app.PeakWorkingSet64
            $metadata.process_peak_working_set_bytes = $peakWorkingSet
            $metadata.process_peak_working_set_mib = [Math]::Round($peakWorkingSet / 1MB, 2)
        } catch {
            $metadata.process_peak_working_set_detail = $_.Exception.Message
        }
    } elseif ($null -ne $startFailure) {
        $metadata.application_start_failure = $startFailure
    }

    if ($presentMonStarted) {
        while (-not $presentMonProcess.WaitForExit(1000)) { }
        $presentMonProcess.Refresh()
        $metadata.presentmon.exit_code = $presentMonProcess.ExitCode
        $metadata.presentmon.csv_created = Test-Path -LiteralPath $presentMonCsvPath -PathType Leaf
        if ($metadata.presentmon.exit_code -eq 0 -and $metadata.presentmon.csv_created) {
            $metadata.presentmon.status = 'collector_completed'
            $metadata.presentmon.detail = 'PresentMon collector exited successfully. The raw CSV is retained; it is not represented as proof of display performance without reviewing its displayed-frame metrics.'
        } else {
            $presentMonOutput = New-Object 'System.Collections.Generic.List[string]'
            foreach ($path in @($presentMonStdoutPath, $presentMonStderrPath)) {
                if (Test-Path -LiteralPath $path -PathType Leaf) {
                    foreach ($line in [System.IO.File]::ReadAllLines($path)) { $presentMonOutput.Add($line) }
                }
            }
            $presentMonDetail = ($presentMonOutput.ToArray() -join "`n").Trim()
            if ([string]::IsNullOrWhiteSpace($presentMonDetail)) {
                $presentMonDetail = "PresentMon exited with code $($metadata.presentmon.exit_code); see the retained stdout/stderr files."
            }
            if ($presentMonDetail.Length -gt 2000) { $presentMonDetail = $presentMonDetail.Substring(0, 2000) }
            $presentMonUnavailableReason = $presentMonDetail
            $metadata.presentmon.status = 'unavailable'
            $metadata.presentmon.detail = $presentMonDetail
            $metadata.presentmon.permission_denied_or_unavailable = $true
        }
    }

    $invalidReasons = New-Object 'System.Collections.Generic.List[string]'
    if ($null -eq $metadata.process_exit_code) { $invalidReasons.Add('Application exit code is unavailable.') }
    if ($metadata.Contains('watchdog_terminated')) { $invalidReasons.Add('Application exceeded its bounded run deadline.') }
    if ($null -ne $startFailure) { $invalidReasons.Add("Application process could not start: $startFailure") }
    if ($null -ne $metadata.process_exit_code -and $metadata.process_exit_code -ne 0) {
        $invalidReasons.Add("Application exited with nonzero code $($metadata.process_exit_code).")
    }
    if (-not (Test-Path -LiteralPath $csvPath -PathType Leaf)) {
        $invalidReasons.Add('Application CSV log was not created.')
    }

    $sampleRows = New-Object 'System.Collections.Generic.List[object]'
    $automaticPauseEvents = New-Object 'System.Collections.Generic.List[object]'
    $deadlineExitTime = $null
    if (Test-Path -LiteralPath $csvPath -PathType Leaf) {
        $csvLines = [System.IO.File]::ReadAllLines($csvPath)
        $dataLines = New-Object 'System.Collections.Generic.List[string]'
        foreach ($line in $csvLines) {
            if (-not [string]::IsNullOrWhiteSpace($line) -and -not $line.StartsWith('#')) {
                $dataLines.Add($line)
            } elseif ($line -match '^# event elapsed_seconds=([0-9]+(?:\.[0-9]+)?) automatic_pause_reason=([^\s]+)') {
                $eventTime = Convert-InvariantDouble $Matches[1]
                $reason = $Matches[2]
                if ($reason -ne 'none') {
                    $automaticPauseEvents.Add([ordered]@{ elapsed_seconds = $eventTime; reason = $reason })
                }
            }
            if ($line -match '^# event elapsed_seconds=([0-9]+(?:\.[0-9]+)?) seconds_deadline_exit$') {
                $deadlineExitTime = Convert-InvariantDouble $Matches[1]
            }
            if ($line -match '^# requested_workers=([0-9]+)') {
                $metadata['csv_requested_workers'] = [int]$Matches[1]
            } elseif ($line -match '^# configured_workers=([0-9]+)') {
                $metadata['csv_configured_workers'] = [int]$Matches[1]
            }
        }
        if ($dataLines.Count -ge 2) {
            $parsedRows = @($dataLines.ToArray() | ConvertFrom-Csv)
            $sampleStart = [double]$effectiveWarmup
            $sampleEnd = [double]($effectiveWarmup + $SampleSeconds)
            foreach ($row in $parsedRows) {
                $elapsedSeconds = Convert-InvariantDouble $row.elapsed_seconds
                if ([double]::IsNaN($elapsedSeconds)) { continue }
                if ($elapsedSeconds -ge $sampleStart -and $elapsedSeconds -lt $sampleEnd) {
                    $sampleRows.Add($row)
                }
            }

            $requiredColumns = @(
                'elapsed_seconds', 'physical_width', 'physical_height', 'logical_width', 'logical_height',
                'simulation_steps_since_previous', 'simulation_ms', 'submit_interval_ms', 'frame_cpu_ms',
                'draw_call_ms', 'submitted_frame_count', 'total_simulation_steps', 'birds_active', 'paused',
                'dropped_simulation_steps', 'dropped_frame_deadlines', 'entering', 'leaving', 'dying',
                'total_birds', 'target_birds'
            )
            $headerColumns = @($dataLines[0].Split(','))
            foreach ($column in $requiredColumns) {
                if ($headerColumns -notcontains $column) {
                    $invalidReasons.Add("Application CSV is missing required column '$column'.")
                }
            }
        } else {
            $invalidReasons.Add('Application CSV contains no frame rows.')
        }
    }
    if ($null -eq $deadlineExitTime -or $deadlineExitTime -lt ($effectiveWarmup + $SampleSeconds)) {
        $invalidReasons.Add('The complete sample endpoint was not reached by the application timed-exit event.')
    }
    $metadata['completion_deadline_elapsed_seconds'] = $deadlineExitTime

    if ($automaticPauseEvents.Count -gt 0) {
        $invalidReasons.Add('An automatic pause occurred during the application run.')
    }
    $metadata.automatic_pause_events = $automaticPauseEvents.ToArray()

    $hasPopulationDeficit = $false
    $hasTargetMismatch = $false
    $hasInvalidTargetValue = $false
    $hasInvalidPopulationValue = $false
    $populationValues = New-Object 'System.Collections.Generic.List[int]'
    foreach ($row in $sampleRows) {
        $pausedValue = 0
        if (-not [int]::TryParse([string]$row.paused, [ref]$pausedValue)) {
            $invalidReasons.Add('A sample row has an invalid paused value.')
        } elseif ($pausedValue -ne 0) {
            $invalidReasons.Add('The application was manually paused during the sample window.')
        }

        $actualTarget = 0
        if ([int]::TryParse([string]$row.target_birds, [ref]$actualTarget)) {
            if ($actualTarget -ne $count) { $hasTargetMismatch = $true }
        } else {
            $hasInvalidTargetValue = $true
        }
        $totalBirds = 0
        if ([int]::TryParse([string]$row.total_birds, [ref]$totalBirds)) {
            $populationValues.Add($totalBirds)
            if ($totalBirds -lt $count) { $hasPopulationDeficit = $true }
        } else {
            $hasInvalidPopulationValue = $true
        }

    }

    if ($hasPopulationDeficit) { $invalidReasons.Add('Actual total bird population fell below the requested target during the sample window.') }
    if ($hasTargetMismatch) { $invalidReasons.Add('The application target_birds value changed during the sample window.') }
    if ($hasInvalidTargetValue) { $invalidReasons.Add('A sample row has an invalid target_birds value.') }
    if ($hasInvalidPopulationValue) { $invalidReasons.Add('A sample row has an invalid total_birds value.') }
    $minimumSampleRows = [Math]::Max(30, $SampleSeconds)
    if ($sampleRows.Count -lt $minimumSampleRows) {
        $invalidReasons.Add("Insufficient frame log rows in the sample window: $($sampleRows.Count) found; at least $minimumSampleRows required.")
    }
    if ($sampleRows.Count -gt 1) {
        $firstSampleElapsed = Convert-InvariantDouble $sampleRows[0].elapsed_seconds
        $lastSampleElapsed = Convert-InvariantDouble $sampleRows[$sampleRows.Count - 1].elapsed_seconds
        $sampleCoverage = ($lastSampleElapsed - $firstSampleElapsed) / [double]$SampleSeconds
    } else {
        $firstSampleElapsed = $null
        $lastSampleElapsed = $null
        $sampleCoverage = 0.0
    }
    if ($sampleCoverage -lt 0.9) {
        $invalidReasons.Add("Sample log time coverage was below 90%: $([Math]::Round($sampleCoverage, 4)).")
    }

    $sampleMetadata = [ordered]@{
        window_start_elapsed_seconds = $effectiveWarmup
        window_end_elapsed_seconds = $effectiveWarmup + $SampleSeconds
        row_count = $sampleRows.Count
        first_row_elapsed_seconds = $firstSampleElapsed
        last_row_elapsed_seconds = $lastSampleElapsed
        time_coverage_fraction = [Math]::Round($sampleCoverage, 6)
        population_at_target_for_every_sample_row = (-not $hasPopulationDeficit -and -not $hasInvalidPopulationValue -and -not $hasTargetMismatch)
        min_total_birds = $(if ($populationValues.Count -gt 0) { ($populationValues.ToArray() | Measure-Object -Minimum).Minimum } else { $null })
        max_total_birds = $(if ($populationValues.Count -gt 0) { ($populationValues.ToArray() | Measure-Object -Maximum).Maximum } else { $null })
    }
    $metadata.sample = $sampleMetadata
    $metadata.invalid_reasons = $invalidReasons.ToArray()
    $metadata.status = $(if ($invalidReasons.Count -eq 0) { 'valid' } else { 'invalid' })
    $metadata.completed_utc = [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
    [System.IO.File]::WriteAllText($metadataPath, ($metadata | ConvertTo-Json -Depth 10), $utf8NoBom)
    $results.Add($metadata)

    if ($metadata.status -eq 'valid') {
        Write-Host "Valid run: count=$count; threads=$Threads; sample rows=$($sampleRows.Count); minimum actual population=$($sampleMetadata.min_total_birds)"
    } else {
        Write-Warning "Invalid run: count=$count; threads=$Threads; reasons=$($invalidReasons -join ' | ')"
    }
    Write-Host "Run data: $runDirectory"
}

$summaryPath = Join-Path $outputRoot 'capacity-run-summary.json'
$summary = [ordered]@{
    generated_utc = [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
    requested_threads = $Threads
    requested_fps_cap = $Fps
    log_flush_behavior = 'buffered_capacity_measurement'
    log_flush_note = 'Per-frame rows and event comments are buffered, but formatting and buffered file I/O still add measurement overhead; logging is not zero-overhead.'
    fps_cap_note = 'Application submission cap only. No actual display FPS is claimed.'
    runs = $results.ToArray()
}
[System.IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 12), $utf8NoBom)
Write-Host "Run summary: $summaryPath"
