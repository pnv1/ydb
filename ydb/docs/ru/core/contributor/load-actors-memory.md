# MemoryLoad

Аллоцирует блоки памяти указанного размера через заданные промежутки времени. После снятия нагрузки аллоцированная память освобождается. С помощью этого актора вы можете протестировать работу логики, например срабатывание некоторого триггера при достижении лимита по [RSS](https://ru.wikipedia.org/wiki/Resident_set_size).

{% note info %}

Это узкоспециализированный актор для тестирования конкретной функциональности. Не является нагружающим, его назначение — проверка корректности работы.

{% endnote %}

## Параметры актора {#options}

| Параметр          | Описание                                                       |
|-------------------|----------------------------------------------------------------|
| `DurationSeconds` | Продолжительность нагрузки в секундах.                         |
| `BlockSize`       | Размер аллоцируемого блока в байтах.                           |
| `IntervalUs`      | Интервал времени между аллоцированиями блоков в микросекундах. |

## Примеры {#examples}

Следующий актор аллоцирует блоки по `1048576` байт каждые `9000000` микросекунд в течение `3600` секунд и за время работы займет 32 ГБ:

```proto
MemoryLoad: {
    DurationSeconds: 3600
    BlockSize: 1048576
    IntervalUs: 9000000
}
```
