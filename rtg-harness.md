# Implementação de uma placa RTG mínima para o Harness

## Objetivo

Implementar uma placa RTG **mínima**, exclusivamente para uso no harness, priorizando desempenho e simplicidade. O objetivo **não** é reproduzir nenhuma placa existente (Picasso IV, Z3660, CyberVision etc.), mas apenas fornecer uma implementação compatível com o Picasso96 suficiente para executar aplicações RTG.

Esta implementação servirá como base para futuras evoluções.

---

## Princípios

* O framebuffer **não deve** ser tratado como um dispositivo MMIO.
* O framebuffer deve ser mapeado como memória normal do emulador.
* Apenas os registradores de controle devem utilizar callbacks MMIO.
* A apresentação da imagem deve ocorrer apenas quando necessário (VBL, Present ou refresh periódico), nunca durante escritas individuais.

O objetivo é eliminar qualquer overhead por acesso ao framebuffer.

---

## Arquitetura desejada

```
                AmigaOS
                    │
                Picasso96
                    │
             Bellatrix RTG
            ┌────────┴────────┐
            │                 │
      MMIO Registers      Linear Framebuffer
            │                 │
     poucas leituras       acesso direto
      e escritas          pela CPU emulada
            │                 │
            └────────┬────────┘
                     │
             Harness / SDL Output
```

---

## Framebuffer

O framebuffer deve ser uma região contínua de memória hospedada diretamente pelo ARM.

Exemplo conceitual:

```c
uint8_t *rtg_vram;
```

O acesso da CPU emulada deve ser traduzido diretamente para essa memória, evitando callbacks para cada leitura ou escrita.

Escritas em pixels devem possuir custo equivalente a um acesso normal de memória.

---

## Registradores

Os únicos acessos MMIO devem ser os registradores de configuração.

Inicialmente são suficientes:

* largura
* altura
* pitch
* formato de pixel
* endereço do framebuffer
* enable
* present/update

Não implementar registradores desnecessários.

---

## Formatos suportados

Na primeira versão basta suportar:

* RGB565

Opcionalmente:

* XRGB8888

Não implementar múltiplos formatos enquanto não forem necessários.

---

## Atualização da tela

A janela do harness não deve ser atualizada durante escritas no framebuffer.

A atualização deve ocorrer apenas:

* uma vez por frame
* quando houver solicitação explícita de apresentação
* ou em uma frequência fixa (50/60 Hz)

Evitar qualquer cópia de framebuffer fora desse momento.

---

## O que NÃO implementar

Nesta primeira versão não implementar:

* aceleração 2D
* blitter
* DMA
* cursor por hardware
* overlays
* interrupts específicos da placa
* sincronização de clocks
* temporização de barramento
* dirty rectangles complexos

O objetivo é apenas disponibilizar um framebuffer linear funcional.

---

## Desempenho

O código deve ser projetado para minimizar overhead.

Evitar:

* callbacks por pixel
* callbacks por word
* callbacks por longword
* logs durante acessos ao framebuffer
* conversões de formato em cada escrita
* cópias completas da VRAM durante a execução

O framebuffer deve permanecer residente e ser apresentado diretamente pelo backend gráfico do harness.

---

## Integração

A placa RTG deve ser completamente independente do Rigel.

Ela não deve participar da emulação do chipset clássico nem do mecanismo de sincronização do Amiga.

Sua única responsabilidade é disponibilizar memória de vídeo linear para o Picasso96 e permitir que o harness apresente esse conteúdo.

---

## Objetivo final

O resultado esperado é uma implementação extremamente simples, com poucas centenas de linhas de código, capaz de:

* abrir uma tela RTG no Amiga;
* permitir desenho por software através do Picasso96;
* apresentar o framebuffer no harness com o menor overhead possível;
* servir como base para futuras otimizações e recursos acelerados.
