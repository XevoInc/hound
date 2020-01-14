'''
A format checker that checks that variable length driver formats are parseable.
Specifically, variable length formats need to be the last specified format in
the format list, or else consumers won't be able to tell one format from
another.
'''

import jsonschema


FORMAT_NAME = 'varlen-fmt'


def check(fmt_list):
    '''Returns True if the format list has no variable length formats other
    than possibly the last format in the list. Raises FormatError otherwise.'''

    for i, fmt in enumerate(fmt_list):
        try:
            size = fmt['size']
        except KeyError:
            continue

        if size == 0 and i != len(fmt_list)-1:
            raise jsonschema.exceptions.FormatError(
    '''\
    variable-length (size 0) property must be the last format
    specified but is not
    fmt: %s''' % fmt)

    return True
