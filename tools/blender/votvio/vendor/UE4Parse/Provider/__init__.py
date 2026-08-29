# VotvIO: MappingProvider needs the Usmap package (UE5 mappings) - not used for UE4.27.
try:
    from .MappingProvider import MappingProvider, PropMappings
except ImportError:
    MappingProvider = None
    PropMappings = None
from .DefaultFileProvider import DefaultFileProvider
