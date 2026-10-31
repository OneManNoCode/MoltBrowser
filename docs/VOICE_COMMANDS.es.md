# Qué puedes decir: las órdenes de voz de MoltBrowser

El modo de voz te deja usar MoltBrowser solo con la voz (el diseño está en
[docs/design/TOUCHLESS_BROWSING.md](design/TOUCHLESS_BROWSING.md), en
inglés). Esta página es la versión en español de
[VOICE_COMMANDS.md](VOICE_COMMANDS.md): enumera las órdenes que el navegador
entiende directamente, sin su modelo de IA, y que funcionan al momento,
incluso en un ordenador lento. Lo demás que digas va a la IA del
dispositivo, que intenta entender qué quieres a partir de la página.

El modo de voz es una vista previa, detrás de una opción experimental.

## El español en el modo de voz

1. Activa `chrome://flags/#molt-voice-mode` y reinicia el navegador.
2. En **Configuración** (`molt://ai-settings`), "Voice mode", elige
   **Español** en "Language" (o deja "The browser's language" si el
   navegador ya está en español).
3. Desde entonces, el modo de voz entiende las órdenes en español y responde
   en español, con una voz en español del sistema (en macOS y Windows
   vienen voces en español; en Linux, una voz de speech-dispatcher). Si el
   ordenador no tiene ninguna, la configuración lo dice: añade una en los
   ajustes de voz del sistema.

**Todavía no escucha en español.** MoltBrowser aún no incluye un modelo de
reconocimiento de voz en español cuya licencia permita distribuirlo con el
navegador, así que con el español elegido el micrófono no se abre y el
navegador lo dice. Mientras tanto puedes **escribir** las órdenes en español
en el campo "Type what you would say" del modo Agente: el navegador las hace
en la pestaña en la que estás y te responde en voz alta.

Cuando escuche en español, la frase de activación será **"Oye Molt"** (también
valen "Hola Molt" y "Hey Molt").

## Cómo hablarle

- **Dilo a tu manera.** "Por favor", "¿puedes...?", "quiero..." y "gracias"
  no estorban. Las mayúsculas, las tildes y la puntuación dan igual.
- **Los verbos** valen como se los dirías a alguien ("abre", "busca") o en
  infinitivo ("abrir", "buscar").
- **Los números** se pueden decir o escribir con cifras: "el cuarto", "la
  cuarta", "el 4º", "el número cuatro", "el cuatro" (justo después de una
  lista), "veintiuno", "treinta y uno", "ciento uno". "El último", "la
  penúltima" y "el segundo desde el final" también.
- **Los tiempos**, como los dirías: "30 segundos", "medio minuto", "un minuto
  y medio", "dos minutos y treinta segundos".
- **Las direcciones** se pueden decir: "ejemplo punto com". El nombre de un
  sitio también sirve: "abre YouTube", "abre la web de Wikipedia".
- **Para elegir de una lista**: después de que el navegador lea una lista, o
  con los números en la página, di un número ("cuatro"), una posición ("el
  último") o lo que dice el elemento ("el de Queen en directo"). Si encajan
  dos, el navegador pregunta cuál.
- **Si te entendió mal**, di "no, dije ..." y lo que querías.
  "Cancela", "para" y "atrás" funcionan siempre.
- **"¿Qué puedo decir?"** enumera lo que encaja en la página en la que estás.

Una orden que compraría, enviaría, publicaría, borraría o compartiría algo se
lee antes y espera tu "sí" (también con los botones en español: "Comprar",
"Pagar", "Eliminar", "Enviar", "Cerrar sesión"...). El texto que una página
muestra o reproduce nunca se toma como una orden.

## Cómo leer las listas de abajo

Cada orden muestra algunas formas de decirla:

- `(abre | ve a)`: di una de estas.
- `[el | la]`: estas palabras se pueden omitir.
- `{site}`: el nombre de un sitio o su dirección dicha. `{address}`: solo una
  dirección dicha ("ejemplo punto com").
- `{query}`, `{text}`, `{name}`: tus propias palabras (una búsqueda, un texto
  que escribir, el nombre de una rutina o de una sección).
- `{seconds}`: un tiempo; `{seconds?}` se puede omitir.
- `{level}`: un volumen, de 0 a 10, en porcentaje, o "la mitad" o "el
  máximo".
- `{pages?}`: cuánto desplazarse ("un poco", "media página", "una página",
  "3 páginas"), o nada.
- `{position}`: una posición o un número ("el cuarto", "el número 12", "el
  siguiente"). `{number}`: lo mismo, o un número solo ("cuatro").
- `{choice}`: cualquier cosa que elija un elemento: una posición, un número
  o lo que dice ("el de Queen en directo", "el botón de enviar").
- `{pick}`: una elección dicha sola justo después de una lista: un número o
  una descripción corta.
- `{tab}`: una pestaña por número, posición o lo que muestra.

Las palabras aparecen como el navegador las compara: en minúsculas, sin
tildes y con "n" por "ñ" ("pestana" es "pestaña", "ultimo" es "último"). El
navegador prueba las formas en el orden de la lista y se queda con la
primera que encaja con todo lo que dijiste.

<!-- BEGIN GENERATED: tools/voice/voice_commands_doc.py --write -->

## Ir a sitios

**Abre un sitio por su nombre o su dirección.** Por ejemplo: "abre YouTube".

- `(abre | abrir | abra | ve a | ir a | vaya a | llevame a | visita | navega a | carga | lanza | entra en | entrar en | entra a) {site} en [una] (nueva pestana | pestana nueva | otra pestana)`
- `(abre | abrir | abra | ve a | ir a | vaya a | llevame a | visita | navega a | carga | lanza | entra en | entrar en | entra a) {site}`
- `{address}`

**Busca: en el sitio en el que estás (o en el buscador si no tiene cuadro de búsqueda), en la web, o en el sitio que digas.** Por ejemplo: "busca Bohemian Rhapsody".

- `(busca | buscar | busque) en (internet | la web | la red) {query}`
- `(busca | buscar | busque) {query} en (internet | la web | la red)`
- `(busca | buscar | busque) en (este sitio | esta web | esta pagina web | aqui) {query}`
- `(busca | buscar | busque) {query} (en este sitio | en esta web | aqui)`
- `en {site} (busca | buscar | busque) {query}`
- `(busca | buscar | busque | buscame) {query} en {site}`
- `(busca | buscar | busque | buscame | encuentra | encuentrame) {query}`

**Encuentra palabras en esta página.** Por ejemplo: "busca horario en esta página".

- `(busca | buscar | encuentra | encontrar) [la palabra] {query} en (esta | la) pagina`
- `(busca | buscar | encuentra) en (esta | la) pagina {query}`

**Vuelve a la página anterior.** Por ejemplo: "atrás".

- `(ve | ir | vuelve | volver | regresa | regresar | llevame) (atras | hacia atras | a la pagina anterior)`
- `(atras | vuelve | volver | regresa | regresar | pagina anterior | retrocede | retroceder)`

**Avanza otra vez.** Por ejemplo: "adelante".

- `(ve | ir | avanza | avanzar) (adelante | hacia adelante)`
- `(adelante | avanza | avanzar)`

**Vuelve a cargar la página.** Por ejemplo: "recarga".

- `(recarga | recargar | recargue | actualiza | actualizar | actualice | refresca | refrescar) [la | esta] [pagina | pestana | web]`
- `(vuelve a cargar | carga de nuevo | carga otra vez) [la | esta] [pagina]`

**Deja de cargar la página.** Por ejemplo: "deja de cargar".

- `(para | deten | detener | parar | pare) la carga [de la pagina]`
- `(deja de | para de | deje de) cargar [la pagina]`

**Ve a tu página de inicio.** Por ejemplo: "ve a inicio".

- `(ve a inicio | ir a inicio | inicio | pagina de inicio | pagina principal | llevame a inicio | ve a la pagina de inicio | ir a la pagina de inicio | abre la pagina de inicio | ve a casa)`

**La página siguiente de resultados.** Por ejemplo: "página siguiente".

- `[ve a | ir a | pasa a] [la] (pagina siguiente | siguiente pagina | proxima pagina) [de resultados]`
- `(mas resultados | muestra mas resultados | muestrame mas resultados | carga mas resultados | ver mas resultados)`

**Abre una pestaña nueva.** Por ejemplo: "abre una pestaña nueva".

- `(abre | abrir | abra | crea | crear) [una] (nueva pestana | pestana nueva | pestana | otra pestana)`
- `(nueva pestana | pestana nueva)`

**Cierra esta pestaña, o una pestaña por su número o su nombre.** Por ejemplo: "cierra esta pestaña".

- `(cierra | cerrar | cierre) (esta pestana | la pestana | la pestana actual | pestana)`
- `(cierra | cerrar | cierre) [la] pestana {tab}`
- `(cierra | cerrar | cierre) [la] {tab} pestana`

**Cambia de pestaña: por número, posición o nombre, o la siguiente.** Por ejemplo: "cambia a la pestaña 2".

- `[ve a | cambia a | pasa a | ir a] [la] (siguiente pestana | pestana siguiente | proxima pestana)`
- `[ve a | cambia a | pasa a | ir a | vuelve a] [la] (pestana anterior | anterior pestana)`
- `(cambia | cambiar | ve | ir | pasa | salta) a la pestana {tab}`
- `pestana {tab}`
- `(cambia | cambiar | ve | ir | pasa | salta) a la {tab} pestana`
- `(cambia | cambiar) a {tab}`

## Ver

**Baja: un poco, media página, una página o varias.** Por ejemplo: "baja".

- `(pagina abajo | avance de pagina)`
- `(baja | bajar | baje | desplazate hacia abajo | desplaza hacia abajo | desplazar hacia abajo | ve hacia abajo | mas abajo | abajo | sigue bajando | baja mas) {pages?}`

**Sube.** Por ejemplo: "sube una página".

- `(pagina arriba | retroceso de pagina)`
- `(sube | subir | suba | desplazate hacia arriba | desplaza hacia arriba | desplazar hacia arriba | ve hacia arriba | mas arriba | arriba | sube mas) {pages?}`

**Desplázate a la izquierda.** Por ejemplo: "desplázate a la izquierda".

- `(desplazate | desplaza | desplazar | ve | mueve) (a la | hacia la) izquierda`

**Desplázate a la derecha.** Por ejemplo: "desplázate a la derecha".

- `(desplazate | desplaza | desplazar | ve | mueve) (a la | hacia la) derecha`

**Ve al principio de la página.** Por ejemplo: "ve al principio".

- `[ve | ir | vuelve | volver | sube | subir | desplazate | vete] (al principio | al inicio | arriba del todo | hasta arriba) [de la pagina]`
- `(principio | inicio) de [la] pagina`

**Ve al final de la página.** Por ejemplo: "ve al final".

- `[ve | ir | baja | bajar | desplazate | vete] (al final | abajo del todo | hasta abajo | hasta el final) [de la pagina]`
- `final de [la] pagina`

**Hazlo todo más grande.** Por ejemplo: "acerca".

- `((acerca | acercar | amplia | ampliar | agranda | agrandar) [el zoom | la pagina | el texto | todo] | (aumenta | aumentar) el zoom | mas grande | texto mas grande | letra mas grande | mas zoom | haz zoom | hazlo mas grande)`

**Hazlo todo más pequeño.** Por ejemplo: "aleja".

- `((aleja | alejar | achica | achicar | encoge | encoger) [el zoom | la pagina | el texto | todo] | (reduce | reducir | disminuye | disminuir) el zoom | mas pequeno | texto mas pequeno | letra mas pequena | menos zoom | hazlo mas pequeno)`

**Vuelve al tamaño normal.** Por ejemplo: "restablece el zoom".

- `((restablece | restablecer | reinicia | quita) [el] zoom | (zoom | tamano) (normal | original | por defecto | real))`

## Multimedia

**Reproduce.** Por ejemplo: "reproduce".

- `(reproduce | reproducir | reproduzca | play | dale al play | pon | poner | ponlo | ponla | reproducelo | reproducela | empieza a reproducir) [esto | eso | el video | la cancion | la musica | la pelicula]`

**Pausa.** Por ejemplo: "pausa".

- `(para | deten | detener | parar | pare) (el video | la musica | la cancion | la reproduccion | la pelicula | el audio)`
- `(pausa | pausar | pausalo | pausala | pon pausa | espera | un momento) [el video | la musica | la cancion | la reproduccion | la pelicula | el audio | la lectura | esto | eso]`

**Sigue reproduciendo.** Por ejemplo: "reanuda".

- `(reanuda | reanudar | continua | continuar | sigue | seguir | quita la pausa | sigue reproduciendo | continua reproduciendo) [el video | la musica | la cancion | la reproduccion]`

**Más alto.** Por ejemplo: "sube el volumen".

- `(mas alto | sube el volumen | subele | sube el sonido | subelo | sube volumen | volumen arriba | mas volumen | aumenta el volumen | un poco mas alto | mucho mas alto | mas fuerte | sube un poco el volumen)`

**Más bajo.** Por ejemplo: "baja el volumen".

- `(mas bajo | baja el volumen | bajale | baja el sonido | bajalo | baja volumen | volumen abajo | menos volumen | reduce el volumen | disminuye el volumen | un poco mas bajo | mucho mas bajo | mas flojo | mas suave | baja un poco el volumen)`

**Pon el volumen: de 0 a 10, un porcentaje, la mitad o el máximo.** Por ejemplo: "pon el volumen a 5".

- `[pon | poner | ajusta | ajustar | cambia | deja] [el] volumen [a | al | en] {level}`

**Quita el sonido.** Por ejemplo: "silencia".

- `(silencia | silenciar | silencie | mutea | mutear) [esto | todo | el (sonido | audio | video | volumen) | la (pestana | musica)]`
- `((quita | quitar | apaga | apagar) el (sonido | audio) | sin sonido | sonido (off | desactivado | apagado))`

**Devuelve el sonido.** Por ejemplo: "activa el sonido".

- `((activa | activar | reactiva | pon | enciende | devuelve) el (sonido | audio) | (quita | quitar) el silencio | desmutea | desmutear | sonido (on | activado | encendido))`

**Adelanta, un paso o lo que digas.** Por ejemplo: "adelanta un minuto".

- `(adelanta | adelantar | adelante | avanza | avanzar | avance | salta | saltar | salta hacia adelante | ve hacia adelante) {seconds}`
- `(adelanta | adelantar | adelantalo | salta hacia adelante | avance rapido) {seconds?}`
- `(salta | saltar | saltate) {seconds?}`

**Retrocede en el vídeo.** Por ejemplo: "retrocede 10 segundos".

- `(retrocede | retroceder | retroceda | atrasa | atrasar | vuelve | regresa | salta hacia atras | ve hacia atras) {seconds}`
- `(rebobina | rebobinar | rebobine) {seconds?}`
- `atras {seconds}`

**Reproduce desde el principio.** Por ejemplo: "empieza de nuevo".

- `(reinicia | reiniciar | empieza de nuevo | empieza otra vez | vuelve a empezar | desde el principio | desde el inicio | ponlo otra vez | ponlo de nuevo | reproduce otra vez | reproduce de nuevo | reinicia el video | rebobina hasta el principio | rebobina al principio)`

**El vídeo o la canción siguiente.** Por ejemplo: "siguiente vídeo".

- `[pon | reproduce | salta a | pasa a | ve a] (el | la) (siguiente | proximo | proxima) (video | cancion | tema | pista | episodio | clip)`
- `[pon | reproduce | salta a | pasa a | ve a] (el | la) (video | cancion | tema | pista | episodio | clip) siguiente`
- `(siguiente | proximo | proxima) (video | cancion | tema | pista | episodio | clip)`
- `(salta | saltar | pasa | pasar | omite) (este | esta | el | la) (video | cancion | tema | pista | episodio | clip)`

**El vídeo o la canción anterior.** Por ejemplo: "canción anterior".

- `[pon | reproduce | vuelve a | ve a] (el | la) (video | cancion | tema | pista | episodio | clip) anterior`
- `[pon | reproduce | vuelve a | ve a] (el | la) anterior (video | cancion | tema | pista | episodio | clip)`
- `(video | cancion | tema | pista | episodio | clip) anterior`

**Pantalla completa.** Por ejemplo: "pantalla completa".

- `[pon | poner | ponlo en | ponla en | activa | activar | ve a | entra en | entrar en | cambia a | modo] [la] pantalla completa`
- `(maximiza | maximizar) [el video | el reproductor]`

**Sal de la pantalla completa.** Por ejemplo: "sal de la pantalla completa".

- `(sal de | salir de | quita | quitar | cierra | cerrar | desactiva | desactivar) [la] pantalla completa`
- `(pantalla normal | minimiza | minimizar) [el video | el reproductor]`

**Muestra los subtítulos.** Por ejemplo: "activa los subtítulos".

- `(activa | activar | pon | poner | muestra | mostrar | enciende | encender) [los] subtitulos`
- `subtitulos (activados | si | on)`

**Oculta los subtítulos.** Por ejemplo: "quita los subtítulos".

- `(desactiva | desactivar | quita | quitar | oculta | ocultar | apaga | apagar) [los] subtitulos`
- `(subtitulos (desactivados | no | off) | sin subtitulos)`

**Reproduce más rápido.** Por ejemplo: "acelera".

- `(mas rapido | acelera | acelerar | aceleralo | reproduce mas rapido | aumenta la velocidad | sube la velocidad)`

**Reproduce más lento.** Por ejemplo: "más lento".

- `(mas lento | mas despacio | ralentiza | reproduce mas lento | reduce la velocidad | baja la velocidad)`

**Vuelve a la velocidad normal.** Por ejemplo: "velocidad normal".

- `([pon | reproduce a] [la] velocidad normal | velocidad por defecto | restablece la velocidad)`

**Salta el anuncio, cuando el reproductor lo permita.** Por ejemplo: "salta el anuncio".

- `(salta | saltar | salte | omite | omitir | omita | quita | saltate) [el | este | los | estos] (anuncio | anuncios | publicidad | comercial)`

## Lectura

**Lee la página en voz alta.** Por ejemplo: "lee la página".

- `(lee | leeme | leer | lea) (la | esta | toda la) (pagina | noticia | publicacion) [completa | entera] [en voz alta]`
- `(lee | leeme | leer | lea) (el | este | todo el) (articulo | texto | contenido) [completo | entero] [en voz alta]`
- `(lee | leeme | leer | empieza a leer | comienza a leer | lee todo | leelo | leelo todo | leemelo) [en voz alta]`

**Sigue leyendo desde donde estás.** Por ejemplo: "lee desde aquí".

- `(lee desde aqui | leer desde aqui | sigue leyendo | continua leyendo | lee el resto [de la pagina] | lee lo que queda | lee a partir de aqui | reanuda la lectura | retoma la lectura)`

**Lee los títulos.** Por ejemplo: "lee los títulos".

- `(lee | leeme | lista | di | dime | cuales son) [todos] (los titulos | los encabezados | las secciones | los apartados)`
- `(titulos | encabezados)`

**Lee los enlaces.** Por ejemplo: "lee los enlaces".

- `(lee | leeme | lista | di | dime | cuales son) [todos] los (enlaces | vinculos | links)`

**Vuelve a leer la lista de resultados.** Por ejemplo: "lee los resultados".

- `(lee | leeme | lista | di | dime | cuales son) [todos | todas] (los resultados | las opciones | los elementos | la lista | los videos | las noticias | los articulos | los productos | los resultados de busqueda) [otra vez | de nuevo]`
- `cuales son mis opciones`

**Lee el título de la página.** Por ejemplo: "lee el título".

- `(lee | leeme | dime | di) el titulo [de la pagina | de esta pagina]`
- `(cual es el titulo [de la pagina | de esta pagina] | como se llama (la | esta) pagina)`

**Lee el texto seleccionado.** Por ejemplo: "lee la selección".

- `(lee | leeme | leer) [la | lo] (seleccion | texto seleccionado | texto resaltado | seleccionado | resaltado)`
- `(lee | leeme | leer) lo que (esta | he) (seleccionado | resaltado)`

**Describe lo que hay en la pantalla.** Por ejemplo: "qué hay en la pantalla".

- `(que hay en (la | mi | esta) (pantalla | pagina) | que ves | que puedes ver | describe (la | esta) (pantalla | pagina | vista) | describeme la pantalla | que hay aqui)`

**Pasa al título siguiente.** Por ejemplo: "siguiente título".

- `[ve al | ve a la | salta al | salta a la | pasa al | pasa a la | lee el | lee la] (siguiente | proximo | proxima) (titulo | encabezado | seccion | apartado)`
- `[ve al | ve a la | salta al | salta a la | pasa al | pasa a la | lee el | lee la] (titulo | encabezado | seccion | apartado) siguiente`

**Vuelve al título anterior.** Por ejemplo: "título anterior".

- `[ve al | ve a la | salta al | salta a la | pasa al | pasa a la | lee el | lee la | vuelve al | vuelve a la] (titulo | encabezado | seccion | apartado) anterior`
- `[ve al | ve a la | salta al | salta a la | pasa al | pasa a la | lee el | lee la | vuelve al | vuelve a la] anterior (titulo | encabezado | seccion | apartado)`

**Pasa al enlace siguiente.** Por ejemplo: "siguiente enlace".

- `[ve al | salta al | pasa al | lee el] (siguiente | proximo) (enlace | vinculo | link)`
- `[ve al | salta al | pasa al | lee el] (enlace | vinculo | link) siguiente`

**Vuelve al enlace anterior.** Por ejemplo: "enlace anterior".

- `[ve al | salta al | pasa al | lee el | vuelve al] (enlace | vinculo | link) anterior`
- `[ve al | salta al | pasa al | lee el | vuelve al] anterior (enlace | vinculo | link)`

**Pasa al elemento siguiente de la lista.** Por ejemplo: "siguiente elemento".

- `[ve al | ve a la | salta al | salta a la | pasa al | pasa a la | lee el | lee la] (siguiente | proximo | proxima) (elemento | resultado | entrada | opcion)`
- `[ve al | ve a la | salta al | salta a la | pasa al | pasa a la | lee el | lee la] (elemento | resultado | entrada | opcion) siguiente`
- `siguiente`

**Vuelve al elemento anterior.** Por ejemplo: "elemento anterior".

- `[ve al | ve a la | salta al | salta a la | pasa al | pasa a la | lee el | lee la | vuelve al | vuelve a la] (elemento | resultado | entrada | opcion) anterior`
- `anterior`

**Lee el párrafo siguiente.** Por ejemplo: "siguiente párrafo".

- `[ve al | salta al | pasa al | lee el] (siguiente | proximo) parrafo`
- `[ve al | salta al | pasa al | lee el] parrafo siguiente`

**Lee el párrafo anterior.** Por ejemplo: "párrafo anterior".

- `[ve al | salta al | pasa al | lee el | vuelve al] parrafo anterior`
- `[ve al | salta al | pasa al | lee el | vuelve al] anterior parrafo`

**Lee una sección de la página, por su título.** Por ejemplo: "lee la sección sobre historia".

- `(lee | leeme | ve a | salta a | pasa a | ir a) la (seccion | parte) (sobre | de | que habla de | llamada | titulada) {query}`
- `(lee | leeme | ve a | salta a | pasa a | ir a) el apartado (sobre | de | que habla de | llamado | titulado) {query}`

## Preguntas sobre la página

**Pregunta por la página en la que estás: de qué trata, sus puntos principales, qué dice sobre algo, quién la escribió, el precio. La IA del dispositivo responde a partir del texto de la página, brevemente, y dice cuando la página no lo dice. "Cuéntame más" sigue con los pasajes siguientes.** Por ejemplo: "de qué trata esta página".

- `(cuentame mas | dime mas | que mas dice [la pagina | el articulo | el texto] | sigue contando)`
- `(lee | leeme | dime | dame | di | resume) (los puntos | las ideas | los aspectos) (principales | clave | importantes | mas importantes)`
- `cuales son (los puntos | las ideas | los aspectos) (principales | clave | importantes | mas importantes)`
- `(puntos principales | puntos clave | ideas principales | ideas clave)`
- `que dice [la pagina | esta pagina | el articulo | este articulo | la noticia | esta noticia | el texto | el sitio | la web] (sobre | de | acerca de | respecto a | respecto de) {query}`
- `(dice | menciona | cuenta) (algo | alguna cosa) (sobre | de | acerca de) {query}`
- `(menciona | habla de | habla sobre | trata sobre | trata de) {query}`
- `(dime | leeme) que dice (la pagina | esta pagina | el articulo | este articulo | el texto) (sobre | de | acerca de) {query}`
- `de que (trata | va | habla) [esta pagina | la pagina | este articulo | el articulo | esto | este video | el video | esta web | el texto | este texto | la noticia | esta noticia]`
- `que dice (la pagina | esta pagina | el articulo | este articulo | el texto | la noticia | esta noticia)`
- `(resume | resumir | resumeme | haz un resumen de | hazme un resumen de) [esto | eso | la pagina | esta pagina | el articulo | este articulo | el texto | el video | este video]`
- `(dame | dime | leeme | hazme | haz) un resumen [de (esto | eso | la pagina | esta pagina | el articulo | este articulo | el texto)]`
- `(resumen | un resumen)`
- `quien (escribio | ha escrito | publico | firma) (esto | este articulo | el articulo | la pagina | esta pagina | la noticia | esta noticia)`
- `quien es el (autor | escritor | editor) [de (esto | este articulo | el articulo | la noticia | esta noticia)]`
- `(cuanto cuesta | cuanto vale | que precio tiene | cual es el precio [de (esto | este producto | el producto)]) [esto | este | esta | eso | el producto | este producto]`
- `cuando (se publico | se escribio | se actualizo | fue publicado | fue escrito | fue actualizado) (esto | este articulo | el articulo | la pagina | esta pagina | la noticia | esta noticia)`
- `(preguntale a la pagina | pregunta a la pagina | pregunta al articulo | preguntale al articulo) {query}`
- `segun (la pagina | esta pagina | el articulo | este articulo | la web | la noticia) {query}`

## Elegir

**Elige de lo que hay en la pantalla: por número, por posición o por nombre, con lo que quieres hacer (abrir, reproducir, pulsar, seleccionar, leer, describir).** Por ejemplo: "reproduce el cuarto".

- `(reproduce | reproducir | pon | poner | ponme | mira | ver | escucha | escuchar) {choice}`
- `(abre | abrir | abra | ve a | ir a | entra en | sigue | visita | muestra | muestrame | ensename) {position}`
- `(haz clic en | haz click en | hacer clic en | clic en | click en | pulsa | pulsar | pulse | presiona | presionar | toca | tocar | aprieta | marca | desmarca) {choice}`
- `(selecciona | seleccionar | seleccione | elige | elegir | escoge | escoger | enfoca) {choice}`
- `(lee | leeme) {choice}`
- `(describe | describeme | que es | hablame de | informacion sobre | detalles de | mas sobre) {choice}`
- `(abre | abrir | abra | ve a | ir a | entra en | visita | muestra | muestrame) {choice}` (justo después de que el navegador lea una lista, o con los números a la vista)
- `(abre | abrir | abra | ve a | ir a | entra en | visita | muestra | muestrame) {choice}`
- `{number}` (justo después de que el navegador lea una lista, o con los números a la vista)
- `{position}`
- `{pick}` (justo después de que el navegador lea una lista, o con los números a la vista)

**Pon números en todo lo que se puede elegir.** Por ejemplo: "muestra los números".

- `((muestra | muestrame | mostrar | ensena | pon | activa) [los] (numeros | etiquetas) | (numeros | etiquetas) (activados | on) | numera todo)`

**Quita los números.** Por ejemplo: "oculta los números".

- `((oculta | ocultar | esconde | quita | quitar | desactiva) [los] (numeros | etiquetas) | (numeros | etiquetas) (off | desactivados) | sin numeros)`

## Escribir

**Escribe en el campo seleccionado.** Por ejemplo: "escribe hola mundo".

- `(escribe | escribir | escriba | teclea | teclear | dicta | dictar) {text}`

**Empieza una línea nueva.** Por ejemplo: "nueva línea".

- `(nueva linea | linea nueva | salto de linea | siguiente linea | nuevo parrafo | punto y aparte)` (mientras escribes en un campo)

**Borra la última palabra.** Por ejemplo: "borra la última palabra".

- `(borra | borrar | elimina | eliminar | quita | quitar | deshaz) [la] ultima palabra` (mientras escribes en un campo)
- `borra esa palabra` (mientras escribes en un campo)

**Vacía el campo.** Por ejemplo: "borra el campo".

- `(borra | borrar | vacia | vaciar | limpia | limpiar | elimina | eliminar) (el | este | la | esta) (campo | cuadro | texto | cuadro de texto | cuadro de busqueda | entrada | casilla)` (mientras escribes en un campo)
- `(borra | borralo | borrar | limpia | limpialo | vacia | vacialo) [todo | eso]` (mientras escribes en un campo)

**Lee lo que has escrito.** Por ejemplo: "lee lo que he escrito".

- `(lee | leeme | leer) lo que (he escrito | escribi | he dicho | dije)` (mientras escribes en un campo)
- `(leelo | leemelo) [otra vez | de nuevo]` (mientras escribes en un campo)
- `que (he escrito | escribi)` (mientras escribes en un campo)

**Envía lo que has escrito.** Por ejemplo: "envía".

- `(envia | enviar | envialo | manda | mandar | mandalo | envia el mensaje | busca | buscar | listo | hecho | intro | enter | ya esta)` (mientras escribes en un campo)
- `(pulsa | presiona | dale a) [la tecla] (intro | enter | entrar)`
- `(envia | enviar | manda | mandar) [el] formulario`

## Hablar con el navegador

**Responde a una pregunta: sí.** Por ejemplo: "sí".

- `(si | sip | claro | vale | ok | okay | de acuerdo | correcto | confirmo | confirmar | confirma | afirmativo | por supuesto | desde luego | exacto | eso es | hazlo | adelante | dale | venga | seguro | perfecto | esta bien | muy bien | claro que si | si claro) [hazlo | adelante | eso es | confirmo | por favor | claro | es correcto | si]` (cuando el navegador te ha hecho una pregunta de sí o no)

**Responde a una pregunta: no.** Por ejemplo: "no".

- `(no | nop | negativo | para nada | de ninguna manera | ni hablar | incorrecto | mal) [no | no lo hagas | cancela | cancelar | eso no | ese no | no es eso | no gracias]` (cuando el navegador te ha hecho una pregunta de sí o no)
- `(no lo hagas | no hagas eso | mejor no)` (cuando el navegador te ha hecho una pregunta de sí o no)

**Cancela lo que pediste.** Por ejemplo: "cancela".

- `(cancela | cancelar | cancele | cancelalo | anula | anular | anulalo | olvidalo | olvidate | olvida eso | da igual | no importa | dejalo | dejalo estar | cancela eso)`

**Para: de hablar, y lo que esté en marcha.** Por ejemplo: "para".

- `(para | parar | pare | detente | deten | detenlo | detener | alto | stop | paralo) [ya | todo | eso | ahora]`

**Deja de hablar.** Por ejemplo: "deja de hablar".

- `(deja de | para de | deje de | pare de | dejar de) (hablar | leer | leerme | hablarme | leer en voz alta)`
- `(callate | calla | silencio | basta | ya basta | chis | shh | suficiente)`

**Deshaz la última acción.** Por ejemplo: "deshaz".

- `(deshaz | deshacer | deshaga | deshazlo) [eso | lo ultimo | la ultima accion | el ultimo cambio | lo que hiciste]`

**Dilo otra vez.** Por ejemplo: "repite".

- `(repite | repetir | repita | repitelo | repitemelo | otra vez | de nuevo | como | que dijiste | que has dicho | perdon | perdona | disculpa | mande | que | dilo otra vez | dimelo otra vez | dilo de nuevo | puedes repetir)`

**Escucha lo que puedes decir.** Por ejemplo: "qué puedo decir".

- `(ayuda | ayudame | necesito ayuda | que puedo decir | que puedo hacer | que puedes hacer | que digo | que ordenes hay | que comandos hay | muestra los comandos | muestrame los comandos | lista de comandos | comandos | ordenes | como funciona esto | como funciona)`

**Escucha dónde estás.** Por ejemplo: "dónde estoy".

- `(donde estoy | donde estamos | en que (pagina | sitio | web | pestana) (estoy | estamos) | que (pagina | sitio | web) es (esta | esto | este))`

**Habla más rápido.** Por ejemplo: "habla más rápido".

- `(habla | hablar | hable | lee | leer | lea) (mas rapido | mas deprisa | rapido | deprisa)`

**Habla más despacio.** Por ejemplo: "habla más despacio".

- `(habla | hablar | hable | lee | leer | lea) (mas despacio | mas lento | despacio | lento | mas lentamente)`

**Cuánto dice el navegador: breve, normal o completo, más o menos.** Por ejemplo: "sé breve".

- `(se breve | modo breve | breve | respuestas breves | respuestas cortas | respuestas mas cortas | detalle breve)`
- `(detalle normal | nivel de detalle normal | detalle por defecto)`
- `(detalle completo | todo el detalle | maximo detalle | modo detallado | se detallado | cuentamelo todo | dimelo todo)`
- `(mas detalle | mas detalles | di mas | mas detallado)`
- `(menos detalle | menos detalles | di menos | menos detallado)`

**Corrige lo que se oyó.** Por ejemplo: "no, dije YouTube".

- `no dije eso (dije | quise decir) {text}`
- `no (eso no | esta mal | mal | te equivocaste) (dije | quise decir) {text}`
- `[no] (dije | he dicho | quise decir | queria decir | pedi) {text}`

## Rutinas

**Guarda lo que acabas de hacer como una rutina.** Por ejemplo: "recuerda esto como mis noticias".

- `(recuerda | guarda | memoriza) (esto | eso) como [rutina | una rutina] [llamada] {name}`
- `(recuerdalo | guardalo | memorizalo) como [rutina | una rutina] [llamada] {name}`
- `guarda esta rutina como {name}`

**Ejecuta una rutina por su nombre.** Por ejemplo: "ejecuta mis noticias".

- `(ejecuta | ejecutar | lanza | inicia | haz | pon | corre | arranca) [la | mi] rutina [llamada] {name}`
- `(ejecuta | ejecutar | lanza | corre) {name}`

**Borra una rutina.** Por ejemplo: "olvida mis noticias".

- `(olvida | borra | elimina | quita) [la | mi] rutina [llamada] {name}`
- `(olvida | olvidate de) {name}`

**Escucha tus rutinas.** Por ejemplo: "cuáles son mis rutinas".

- `(cuales | que) son mis rutinas`
- `que rutinas (tengo | hay | he guardado)`
- `(lista | lee | leeme | dime | di) [todas] (las | mis) rutinas`
- `(mis rutinas | rutinas)`

**Olvida una palabra o una corrección que el navegador aprendió de ti.** Por ejemplo: "olvida la palabra Queen".

- `(olvida | borra | elimina | quita) la (palabra | correccion | frase) {name}`
- `(olvida | borra | elimina | quita) el nombre {name}`

**Olvida todo lo que el navegador aprendió de ti: rutinas, palabras, correcciones y pistas de sitios.** Por ejemplo: "olvida todo lo que aprendiste".

- `(olvida | borra | elimina | olvidate de) todo [lo] que (aprendiste | has aprendido | sabes) [de mi | sobre mi]`

## Modo de voz

**Empieza a escuchar (escrito en el panel del modo Agente).** Por ejemplo: "activa el modo de voz".

- `(activa | activar | active | enciende | encender | encienda | inicia | iniciar | entra en | entrar en) [el] (modo de voz | modo voz | control por voz | control de voz)`
- `(modo de voz | modo voz | control por voz) (activado | encendido | on)`
- `(empieza | empezar | comienza | comenzar) a (escuchar | escucharme)`

**Deja de escuchar. El navegador pregunta antes: después, el botón Voz de la barra de herramientas, o la configuración, lo vuelven a activar.** Por ejemplo: "desactiva el modo de voz".

- `(desactiva | desactivar | desactive | apaga | apagar | apague | cierra | cerrar | sal del | salir del | termina | terminar) [el] (modo de voz | modo voz | control por voz | control de voz)`
- `(modo de voz | modo voz | control por voz) (desactivado | apagado | off)`
- `(deja de | para de | deje de) (escuchar | escucharme)`

<!-- END GENERATED -->

## Todavía no

- **Escuchar en español**: falta un modelo de reconocimiento en español que
  se pueda distribuir con MoltBrowser (las órdenes escritas funcionan).
- **La voz propia de MoltBrowser en español**: su voz incluida lee solo
  inglés, así que en español habla una voz del sistema.
- **Algunas respuestas** todavía se dicen en inglés: las de las rutinas y lo
  aprendido, las de los avisos del navegador (permisos, descargas, diálogos
  de las páginas) y las preguntas de la IA del dispositivo.
- **La palabra de confirmación** de los avisos ("di manzana para
  permitirlo") sigue siendo una palabra inglesa por ahora; las palabras en
  español ya están elegidas.
- **Las preguntas sobre la página** se entienden en español, pero la IA del
  dispositivo puede responder en inglés.
