---
name: "mascota-de-escritorio"
description: "Úsala en cada tarea o conversación de trabajo para avisar a la mascota de escritorio de lo que está haciendo Claude (pensando, trabajando, esperando, terminado o con error)."
---

# Mascota de escritorio

En el escritorio de Windows vive una mascota (un centauro nórdico) que enseña en qué está Claude en cada proyecto. La mascota lee este archivo cada pocos segundos:

`C:\RUTA\A\mascota-claude\estado.json`

<!-- Cambia la ruta de arriba por la carpeta donde tengas mascota-claude.exe -->

Tu trabajo es mantener al día la entrada del proyecto en el que estás trabajando. Si no tienes acceso a esa carpeta (por ejemplo, en un chat sin acceso a archivos), no hagas nada y sigue con la tarea con normalidad: nunca interrumpas el trabajo por la mascota ni lo menciones.

## Formato del archivo

```json
{
  "projects": [
    {
      "name": "Informe trimestral",
      "state": "working",
      "status": "Redactando el resumen",
      "updatedAt": "2026-01-01T10:00:00Z",
      "forja": {
        "objeto": "tomo",
        "nombre": "Tomo del informe",
        "piezas": [
          { "nombre": "Recoger datos", "hecha": true },
          { "nombre": "Redactar", "hecha": false },
          { "nombre": "Revisar", "hecha": false }
        ]
      }
    }
  ]
}
```

- `name`: nombre corto del proyecto o de la tarea, siempre el mismo durante toda la conversación (el nombre del proyecto de Claude o de Cowork si lo hay; si no, tres o cuatro palabras que describan la tarea).
- `state`: uno de estos valores exactos:
  - `thinking`: estás leyendo, investigando o planificando.
  - `working`: estás haciendo el trabajo (escribiendo, editando archivos, ejecutando cosas).
  - `waiting`: necesitas que la persona responda, decida o dé permiso.
  - `happy`: has terminado la tarea y ha salido bien.
  - `worried`: algo ha fallado y no puedes seguir sin ayuda.
  - `sleeping`: la conversación ha terminado y no queda nada pendiente.
- `status`: una frase corta en español (máximo unos 50 caracteres) que diga qué haces o qué necesitas. Cuando el estado es `waiting`, escribe la pregunta, por ejemplo "¿Envío el correo?".
- `updatedAt`: fecha y hora actuales en formato ISO.
- `forja` (opcional): el objeto que el centauro va forjando, una pieza por cada paso de la tarea. Úsala cuando la tarea tenga entre 2 y 8 pasos claros. Si la tarea es de un solo paso, no la pongas.
  - `objeto`: elige según el tipo de tarea:
    - `espada`: construir o crear algo (código, una web, un archivo, una automatización).
    - `escudo`: revisar, corregir o comprobar (QA, auditorías, arreglar errores).
    - `tomo`: escribir o documentar (textos, informes, presentaciones, correos).
    - `catalejo`: investigar o buscar información (research, análisis, comparativas).
  - `nombre`: nombre épico y corto del objeto relacionado con la tarea, por ejemplo "Espada del portfolio".
  - `piezas`: un elemento por paso, en orden, con `nombre` (dos a cuatro palabras) y `hecha` (`true` cuando el paso está terminado).

## Cuándo actualizarlo

1. Al empezar una tarea: `thinking`.
2. Al ponerte a hacer el trabajo: `working`, con lo que estás haciendo. Si la tarea tiene varios pasos, añade la `forja` con todas las piezas en `hecha: false`.
3. Cada vez que termines un paso: marca su pieza como `hecha: true` y actualiza `status` con el siguiente paso.
4. Justo antes de hacer una pregunta o pedir permiso: `waiting`, con la pregunta.
5. Al terminar: `happy`, con el resultado en pocas palabras y todas las piezas en `hecha: true`.
6. Si algo falla y te bloquea: `worried`, con qué ha fallado.

Si el plan cambia a mitad, ajusta la lista de piezas. No hace falta actualizarlo en cada paso pequeño.

## Cómo escribirlo sin pisar otros proyectos

1. Lee el archivo justo antes de escribirlo (otros chats lo actualizan a la vez). Si no existe o no es JSON válido, empieza con `{ "projects": [] }`.
2. Busca la entrada con tu `name`. Si está, actualiza `state`, `status`, `updatedAt` y `forja`. Si no está, añádela al principio de la lista.
3. Deja intactas las entradas de los demás proyectos.
4. Si hay más de 8 entradas, quita las que estén en `sleeping` y tengan el `updatedAt` más antiguo.
5. Escribe el archivo completo en UTF-8, sin comentarios.
