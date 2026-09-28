"""Parser for Unreal's exported assembly structs (data only; no eval)."""
import json
import re


def parse_struct(text):
    if len(text) > 8 * 1024 * 1024:
        raise ValueError('Assembly metadata is too large')
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|[(),=]|[^\s(),=]+', text)
    at = 0
    def value(depth=0):
        nonlocal at
        if depth > 48 or at >= len(tokens):
            raise ValueError('Invalid assembly metadata')
        if tokens[at] in (',', ')'):
            return []
        token = tokens[at]; at += 1
        if token != '(':
            if token.startswith('"'): return json.loads(token)
            if token in ('True', 'False'): return token == 'True'
            try: return float(token) if any(c in token for c in '.eE') else int(token)
            except ValueError: return token
        if at < len(tokens) and tokens[at] == ')':
            at += 1
            return []
        mapping = at + 1 < len(tokens) and tokens[at + 1] == '='
        result = {} if mapping else []
        while at < len(tokens) and tokens[at] != ')':
            if mapping:
                key = tokens[at]; at += 1
                if tokens[at] != '=': raise ValueError('Expected assembly field')
                at += 1; result[key] = value(depth + 1)
            else:
                result.append(value(depth + 1))
            if tokens[at] == ',': at += 1
            elif tokens[at] != ')': raise ValueError('Expected assembly delimiter')
        if at >= len(tokens): raise ValueError('Truncated assembly metadata')
        at += 1
        return result
    result = value()
    if at != len(tokens): raise ValueError('Trailing assembly metadata')
    return result

