# Execute no PowerShell, dentro da pasta projeto. Sem bibliotecas adicionais.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('servidor', 'clientes', 'resumo')][string]$Modo,
    [string]$IPServidor,
    [ValidateRange(1, 65535)][int]$Porta = 5000,
    [ValidateRange(1, 10)][int]$MinClientes = 1,
    [ValidateRange(1, 10)][int]$MaxClientes = 10,
    [ValidateRange(1, 10000)][int]$Repeticoes = 5,
    [ValidateRange(1, 3600)][int]$Duracao = 30,
    [ValidateRange(1, 60)][int]$Aquecimento = 3,
    [ValidateRange(1024, 4194304)][int]$Bloco = 65536,
    [string]$Csv = 'resultados.csv'
)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture

if ($Modo -eq 'resumo') {
    $rows = @(Import-Csv -LiteralPath $Csv)
    if ($rows.Count -eq 0) { throw 'CSV sem resultados.' }
    $invalid = @($rows | Where-Object { $_.valido -ne '1' })
    if ($invalid.Count) { Write-Warning "$($invalid.Count) ensaios invalidos excluidos." }
    $valid = @($rows | Where-Object { $_.valido -eq '1' })
    if (!$valid.Count) { throw 'Nenhum ensaio valido.' }
    $settings = @($valid | ForEach-Object {
        '{0}/{1}/{2}' -f $_.bloco_bytes, $_.aquecimento_s, $_.duracao_s
    } | Select-Object -Unique)
    if ($settings.Count -ne 1) { throw 'O CSV mistura duracoes, aquecimentos ou blocos diferentes.' }
    $duplicates = @($valid | Group-Object clientes,repeticao | Where-Object Count -gt 1)
    if ($duplicates.Count) { throw 'Ha repeticoes duplicadas. Separe as campanhas em CSVs diferentes.' }
    $summary = @($valid | Group-Object clientes | Sort-Object { [int]$_.Name } | ForEach-Object {
        $values = @($_.Group | ForEach-Object {
            [double]::Parse($_.throughput_mbps, $culture)
        })
        $stats = $values | Measure-Object -Average -Maximum
        $squared = 0.0
        foreach ($value in $values) { $squared += [Math]::Pow($value - $stats.Average, 2) }
        $deviation = if ($values.Count -gt 1) {
            [Math]::Sqrt($squared / ($values.Count - 1)).ToString('F6', $culture)
        } else { '' }
        [PSCustomObject]@{
            clientes = [int]$_.Name
            repeticoes_validas = $values.Count
            maximo_mbps = $stats.Maximum.ToString('F6', $culture)
            media_mbps = $stats.Average.ToString('F6', $culture)
            desvio_padrao_mbps = $deviation
        }
    })
    $summary | Format-Table -AutoSize
    $summaryPath = [IO.Path]::ChangeExtension([IO.Path]::GetFullPath($Csv), 'resumo.csv')
    $summary | Export-Csv -LiteralPath $summaryPath -NoTypeInformation -Encoding UTF8
    Write-Host "Resumo salvo em $summaryPath"
    return
}

if ($MinClientes -gt $MaxClientes) { throw 'MinClientes deve ser <= MaxClientes.' }
if ($Modo -eq 'clientes' -and !$IPServidor) { throw 'Informe -IPServidor com o IPv4 do servidor.' }
$onWindows = $env:OS -eq 'Windows_NT'
$suffix = if ($onWindows) { '.exe' } else { '' }
$name = if ($Modo -eq 'servidor') { 'chat_server_copy' } else { 'chat_client_copy' }
$binary = Join-Path $PSScriptRoot ($name + $suffix)
if (!(Test-Path -LiteralPath $binary)) { throw "Compile primeiro: $binary" }
$csvPath = [IO.Path]::GetFullPath($Csv)
if ($Modo -eq 'servidor' -and (Test-Path -LiteralPath $csvPath)) {
    throw 'O CSV ja existe. Use -Csv com um novo nome para nao misturar campanhas.'
}
$logDir = Join-Path $PSScriptRoot ('logs_' + $Modo + '_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
New-Item -ItemType Directory -Path $logDir | Out-Null
Write-Host "Logs: $logDir"

# Argumentos de caminhos sao colocados entre aspas para suportar espacos.
function Start-Benchmark([string[]]$Arguments, [string]$Label) {
    $quoted = @($Arguments | ForEach-Object {
        if ($_ -match '["\r\n]') { throw 'Argumento contem aspas ou quebra de linha.' }
        '"' + $_ + '"'
    }) -join ' '
    $parameters = @{
        FilePath = $binary
        ArgumentList = $quoted
        PassThru = $true
        RedirectStandardOutput = (Join-Path $logDir ($Label + '.out.log'))
        RedirectStandardError = (Join-Path $logDir ($Label + '.err.log'))
    }
    if ($onWindows) { $parameters.WindowStyle = 'Hidden' }
    $child = Start-Process @parameters
    # Mantem o handle aberto para que o Windows PowerShell 5.1 preserve ExitCode.
    $null = $child.Handle
    return $child
}

for ($rep = 1; $rep -le $Repeticoes; $rep++) {
    # Alterna ordem crescente/decrescente para reduzir efeitos de ordem.
    $counts = if ($rep % 2) { $MinClientes..$MaxClientes } else { $MaxClientes..$MinClientes }
    foreach ($count in $counts) {
        Write-Host "Cenario: $count clientes, repeticao $rep/$Repeticoes"
        $processes = @()
        try {
            if ($Modo -eq 'servidor') {
                $processes += Start-Benchmark @(
                    "$Porta", "$count", "$Duracao", "$Aquecimento", "$Bloco", $csvPath, "$rep"
                ) "n${count}_r${rep}_servidor"
            } else {
                for ($i = 1; $i -le $count; $i++) {
                    $processes += Start-Benchmark @(
                        $IPServidor, "$Porta", "$count", "$rep"
                    ) "n${count}_r${rep}_cliente${i}"
                }
            }
            # Clientes recebem a duracao oficial do servidor. Aqui, os argumentos
            # iguais nos dois PCs permitem limitar a espera do script tambem.
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $limit = 280 + $Duracao + $Aquecimento
            do {
                $running = 0
                foreach ($process in $processes) {
                    $process.Refresh()
                    if (!$process.HasExited) { $running++ }
                    else {
                        $process.WaitForExit()
                        if ($process.ExitCode -ne 0) {
                            throw "Processo falhou (codigo $($process.ExitCode)). Veja os logs em $logDir"
                        }
                    }
                }
                if ($timer.Elapsed.TotalSeconds -gt $limit) { throw 'Prazo do cenario esgotado.' }
                if ($running) { Start-Sleep -Milliseconds 100 }
            } while ($running)
        } finally {
            foreach ($process in $processes) {
                if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
                $process.Dispose()
            }
        }
        if ($Modo -eq 'clientes') { Start-Sleep -Seconds 1 }
    }
}
Write-Host 'Campanha concluida.'
if ($Modo -eq 'servidor') { Write-Host "Resultados: $csvPath" }
