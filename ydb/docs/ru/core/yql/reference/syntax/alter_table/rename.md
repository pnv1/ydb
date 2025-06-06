# Переименование таблицы

{% if oss == true and backend_name == "YDB" %}

{% include [OLAP_not_allow_note](../../../../_includes/not_allow_for_olap_note.md) %}

{% endif %}

```yql
ALTER TABLE old_table_name RENAME TO new_table_name;
```

{% if oss == true and backend_name == "YDB" %}

{% cut "См. правила наименования таблиц и колонок" %}

{% include [table naming rules](../../../../concepts/datamodel/_includes/object-naming-rules.md) %}

{% endcut %}

{% endif %}

Если таблица с новым именем существует, будет возвращена ошибка. Возможность транзакционной подмены таблицы под нагрузкой поддерживается специализированными методами в CLI и SDK.

{% note warning %}

Если в YQL запросе содержится несколько команд `ALTER TABLE ... RENAME TO ...`, то каждая будет выполнена в режиме автокоммита в отдельной транзакции. С точки зрения внешнего процесса, таблицы будут переименованы последовательно одна за другой. Чтобы переименовать несколько таблиц в одной транзакции, используйте специализированные методы, доступные в CLI и SDK.

{% endnote %}

Переименование может использоваться для перемещения таблицы из одной директории внутри БД в другую, например:

```yql
ALTER TABLE `table1` RENAME TO `/backup/table1`;
```
