# Benchmark TCP entre dois computadores

As copias foram transformadas em um benchmark de recepcao TCP. Os arquivos
originais na pasta superior continuam sendo o chat interativo.

- **PC A:** servidor e armazenamento dos resultados.
- **PC B:** de 1 a 10 processos clientes simultaneos.
- Use o IPv4 do PC A na rede local, nunca `127.0.0.1` no PC B.
- Mantenha as maquinas e a topologia iguais durante toda a campanha.

## Arquivos

- `chat_server_copy.c`: uma thread receptora por cliente, janela comum e CSV.
- `chat_client_copy.c`: envio automatico, incluindo tratamento de envios parciais.
- `benchmark_common.h`: sockets, prazos e relogio para Windows/Linux. Copie junto dos `.c`.
- `experimentos.ps1`: executa os cenarios, guarda logs e calcula o resumo.

## Compilação

Execute os comandos dentro de `projeto`, nos dois computadores. Nao misture
executaveis Windows e Linux: compile para o sistema de cada maquina.

**Windows: GCC do MSYS2/MinGW-w64 (UCRT64), incluindo winpthreads.**
Este ambiente ja foi encontrado no PC usado para modificar o projeto.
No PowerShell, o diretorio `C:\msys64\ucrt64\bin` precisa estar no PATH,
inclusive durante a execucao, por causa das DLLs do compilador.

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -pthread chat_server_copy.c -o chat_server_copy.exe -lws2_32
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic chat_client_copy.c -o chat_client_copy.exe -lws2_32
```

**Linux: GCC e pthreads.**

```bash
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -pthread chat_server_copy.c -o chat_server_copy
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic chat_client_copy.c -o chat_client_copy
```

A automacao requer PowerShell: Windows PowerShell 5.1 ou PowerShell 7. No Linux,
use `pwsh -File ./experimentos.ps1 ...` se ele estiver instalado; os executaveis
C tambem podem ser usados diretamente sem o script.

## Preparacao da rede

1. Conecte os dois PCs a mesma rede. Para reduzir variabilidade, prefira cabo.
2. No servidor, consulte `ipconfig` (Windows) ou `ip -4 addr` (Linux). Selecione
   o IPv4 da interface ligada a rede do cliente.
3. Permita entrada TCP na porta escolhida (exemplo: 5000) no firewall do servidor,
   restrita a rede/maquina do experimento. Nao e necessario desativar o firewall.
4. Inicie primeiro o servidor. No outro PC, inicie os clientes em ate 120 segundos.
5. Se nao conectar, confira IP, porta, firewall e eventual isolamento entre
   dispositivos no Wi-Fi. A comunicacao entre maquinas precisa estar permitida.

## Primeiro teste: um cliente

No **PC A**, PowerShell:

```powershell
.\chat_server_copy.exe 5000 1 30 3 65536 teste_unico.csv 1
```

No **PC B** (substitua o IP pelo IPv4 real do PC A):

```powershell
.\chat_client_copy.exe 192.168.1.100 5000 1 1
```

No Linux, troque `.\programa.exe` por `./programa`.

Argumentos do servidor:

```text
porta clientes segundos [aquecimento=3] [bloco=65536] [csv=resultados.csv] [repeticao=1]
```

Argumentos de cada cliente:

```text
IPv4-servidor porta [clientes-esperados=1] [repeticao=1]
```

O argumento `clientes-esperados` identifica o cenario: **nao cria varios clientes**.
Para um teste manual com N clientes, execute N processos com os mesmos argumentos.
O script faz isso automaticamente. O servidor aceita de 1 a 10 clientes,
medicao de 1 a 3600 segundos, aquecimento de 1 a 60 segundos e blocos de
1024 a 4194304 bytes. Para os testes oficiais, use uma duracao longa como 30 s.

## Campanha completa: 1 a 10 clientes, cinco repeticoes

No **PC A**:

```powershell
.\experimentos.ps1 -Modo servidor -Csv campanha01.csv
```

No **PC B**:

```powershell
.\experimentos.ps1 -Modo clientes -IPServidor 192.168.1.100
```

Os padroes sao 30 s medidos, 3 s de aquecimento, blocos de 65536 bytes e cinco
repeticoes: **50 ensaios**, aproximadamente 28 a 30 minutos sem falhas.
A ordem e 1..10 na primeira repeticao, 10..1 na segunda, e assim por diante.

Para testar apenas dez clientes, uma vez, use nos dois PCs:

```powershell
# PC A
.\experimentos.ps1 -Modo servidor -MinClientes 10 -MaxClientes 10 -Repeticoes 1 -Csv dez_clientes.csv
# PC B
.\experimentos.ps1 -Modo clientes -IPServidor 192.168.1.100 -MinClientes 10 -MaxClientes 10 -Repeticoes 1
```

Os parametros `-Porta`, `-MinClientes`, `-MaxClientes`, `-Repeticoes`, `-Duracao`
e `-Aquecimento` devem ser iguais nos dois lados. O bloco e definido no servidor
com `-Bloco`; ele informa esse valor a cada cliente no protocolo.

Se a politica local bloquear scripts, voce pode usar uma invocacao temporaria,
sem alterar a politica permanente do sistema:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\experimentos.ps1 -Modo servidor -Csv campanha01.csv
```

Use o mesmo formato para `-Modo clientes` no PC B. O script cria uma pasta de
logs por campanha, interrompe ao detectar erros e nao reutiliza um CSV existente.
Se houver falha, interrompa tambem o outro PC, confira os logs e reinicie ambos
com um novo nome de CSV. O executavel servidor, usado diretamente, acrescenta
linhas a um CSV existente; evite misturar campanhas.

## Como a medicao funciona

1. Cada cliente conecta, identifica cenario/repeticao e recebe a configuracao.
2. O cliente aloca e preenche seu bloco apenas uma vez e envia `READY`.
3. O servidor espera todos os clientes e threads estarem prontos. Sob um mutex,
   fixa `inicio = relogio_monotonico + aquecimento` e `fim = inicio + duracao`.
4. Libera as threads, que enviam `GO`. Os clientes transmitem continuamente.
   A liberacao nao e simultanea ao nanossegundo; o aquecimento absorve esse atraso.
5. Cada thread chama `recv()` em blocos e consulta o relogio imediatamente apos
   o retorno. Soma o valor retornado somente se `inicio <= instante < fim`.
6. Ao final, o servidor envia `STOP`. O cliente para de enviar e faz
   `shutdown` apenas da escrita. O servidor drena os bytes restantes, fora da
   contagem, e confirma com `OK` ou `ERROR`.
7. A thread principal usa `pthread_join`, soma os contadores e grava o CSV.

O mutex nao e usado no loop de recepcao. Nao ha broadcast, formatacao de mensagens,
impressao por bloco ou geracao aleatoria durante a transmissao. TCP pode entregar
um bloco em varios `recv()`: sempre se conta o numero real retornado pela chamada.
Sockets nao bloqueantes e `select()` limitam a espera e permitem controlar o fim.

### Formula e significado do resultado

```text
bytes_totais = soma(bytes_recebidos_por_thread)
tempo = fim - inicio
throughput_mbps = bytes_totais * 8 / (tempo * 1000000)
```

`duracao_s` representa essa janela comum do relogio do servidor, e nao o tempo
total do processo (que inclui conexao, aquecimento e drenagem). Os relogios dos
dois computadores nao precisam estar sincronizados. A contagem usa contadores
de 64 bits e exclui mensagens de controle, aquecimento e drenagem.

A fronteira e definida pela conclusao de `recv()`, nao pelo instante fisico de
chegada de cada pacote. Uma chamada que cruza a fronteira e integralmente
incluida ou excluida conforme seu instante de conclusao; documente essa limitacao.
O resultado e o **goodput da aplicacao**, sem cabecalhos TCP/IP/Ethernet.
Os bytes mostrados no cliente incluem aquecimento e nao sao o resultado oficial.

Uma desconexao antecipada, erro, ausencia de bytes medidos ou falha no encerramento
invalida o teste (`valido=0`, quando a medicao chegou a ser executada). Falhas
antes do inicio nao geram linha de resultado. O script interrompe a campanha;
consulte os logs. Uma conexao lenta nao e automaticamente considerada invalida.

## Resumo para o relatorio

No servidor, depois da campanha:

```powershell
.\experimentos.ps1 -Modo resumo -Csv campanha01.csv
```

Gera `campanha01.resumo.csv` com quantidade de clientes, numero de repeticoes
validas, **maximo agregado**, media e desvio-padrao amostral (vazio com apenas
uma repeticao). Ensaios invalidos sao excluidos com aviso. Confira que os dez
cenarios possuem cinco repeticoes validas antes de entregar o trabalho.

No Excel ou LibreOffice, importe o CSV usando virgula como separador e ponto
como separador decimal. Crie um grafico de dispersao com linhas:

- X: `clientes`.
- Y: `maximo_mbps`.
- Opcional: outro grafico com media e barras de desvio-padrao.

O maximo e o maior throughput medio de um ensaio completo entre as repeticoes,
nao um pico instantaneo. Aumentar o numero de clientes pode saturar a placa de
rede, CPU ou memoria de **qualquer um dos dois PCs**, alem do switch/AP.
Registre CPU por nucleo e uso das interfaces para fundamentar a analise.

## Informacoes a registrar nos dois PCs

Windows (PowerShell):

```powershell
Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors
Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory
Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,OSArchitecture
Get-NetAdapter | Select-Object Name,InterfaceDescription,Status,LinkSpeed
Get-NetIPConfiguration
gcc --version
```

Linux:

```bash
lscpu
free -h
cat /etc/os-release
uname -r
ip -4 addr
ip link
gcc --version
```

Registre tambem cabo/Wi-Fi, velocidade negociada, modelo do switch/roteador,
topologia, MTU, virtualizacao, flags de compilacao, data e versao do codigo.
Nao altere bloco ou duracao entre os cenarios de uma mesma campanha. Feche
aplicacoes concorrentes e mantenha ambos os PCs ligados a energia.

O relatorio final ainda precisa dos resultados **reais entre os dois PCs**,
graficos, discussao e identificacao dos integrantes. Publique os fontes, este
README e os dados da campanha no GitHub e inclua os links reais no relatorio.
Nenhum repositorio foi criado ou publicado automaticamente.

## Verificacao do funcionamento

Nesta implementacao, a compilacao Windows com GCC 16.2.0 passou tambem com
`-Werror`. Foram executados testes curtos em loopback com 1 a 10 clientes,
repeticoes em ordem crescente/decrescente, blocos de 1024 e 65536 bytes,
conferencia da soma dos contadores e da formula, desconexao antecipada,
cenario incorreto e argumentos invalidos. A versao final foi novamente
verificada com dez clientes. Os artefatos locais ficam em `verificacao_local/`.

Esses testes verificam o programa, **nao substituem a campanha entre dois PCs**
e nao devem ser usados como resultados de rede no relatorio. O caminho Linux
foi implementado, mas nao foi compilado/executado neste ambiente Windows.
