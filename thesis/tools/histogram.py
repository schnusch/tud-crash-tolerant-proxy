from decimal import Decimal
from typing import List


def median(values: List[Decimal]) -> Decimal:
    return (values[(len(values) - 1) // 2] + values[len(values) // 2]) / 2
